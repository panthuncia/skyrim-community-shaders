// The build pump and the pipeline lane.

#include "Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.h"

#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "RenderGraph/Async/Lanes.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

#include <ORGModuleServices/Async/SerializedTaskPump.h>
#include <tbb/concurrent_queue.h>

namespace DCLF
{
	namespace
	{
		/**
		 * @brief A lock-free, width-limited pump onto the preparation pool. A runner gives its slot back only once the count reads zero,
		 * then looks once more, so a push that found every slot taken is never left without a runner.
		 */
		struct BuildPump
		{
			using Task = std::function<void()>;

			tbb::concurrent_queue<Task> tasks;
			// A pop may precede its push's count, so this can read below zero for a moment.
			std::atomic<std::int64_t> queued{ 0 };
			std::atomic<unsigned> runners{ 0 };
			const unsigned width = BuildWidth();

			bool Submit(Task a_task)
			{
				tasks.push(std::move(a_task));
				queued.fetch_add(1);
				if (TakeRunner())
					Dispatch();
				return true;
			}

			bool TakeRunner()
			{
				for (unsigned running = runners.load(); running < width;)
					if (runners.compare_exchange_weak(running, running + 1))
						return true;
				return false;
			}

			// Holds a runner slot.
			void Dispatch()
			{
				::RenderGraph::Frame::SharedExecutor().Dispatch(SceneScheduler::Scope(), ::RenderGraph::Frame::Executor::Preparation, org::async::TaskDispatch::Cpu, "DCLF builds",
					[this](const org::async::TaskContext&) { Run(); });
			}

			void Run()
			{
				if (Task task; tasks.try_pop(task)) {
					queued.fetch_sub(1);
					// ORG's tasks report failures through their artifacts. Nothing may stop the pump.
					try {
						task();
					} catch (...) {
					}
				}
				if (queued.load() > 0) {
					Dispatch();  // keeps the slot, behind whatever the pool queued meanwhile
					return;
				}
				runners.fetch_sub(1);
				// Catch a push that counted after the read above and found every slot taken.
				if (queued.load() > 0 && TakeRunner())
					Dispatch();
			}
		};

		BuildPump& Pump()
		{
			// Never destroyed, because builds may still complete while the process tears down.
			static auto* pump = new BuildPump;
			return *pump;
		}
	}

	unsigned BuildWidth()
	{
		return (std::max)(1u, ::RenderGraph::Frame::PreparationWorkers() / 2);
	}

	std::function<bool(std::function<void()>)> BuildSubmitter()
	{
		return [](std::function<void()> a_task) { return Pump().Submit(std::move(a_task)); };
	}

	namespace
	{
		std::atomic<void (*)()> laneDrain{ nullptr };

		org::async::SerializedTaskPump& Lane()
		{
			// Never destroyed, because completions may still arrive while the process tears down.
			static auto& lane = ::RenderGraph::Frame::MakeLane(
				{ "DCLF pipeline lane", ::RenderGraph::Frame::Executor::Coordinator, org::async::TaskDispatch::Cpu,
					[] { logger::error("[DCLF] the pipeline lane was refused by the executor; no Lighting pipeline is admitted from here on"); } },
				[] {
					if (auto* drain = laneDrain.load(std::memory_order_acquire))
						drain();
				});
			return lane;
		}
	}

	void SetPipelineLaneDrain(void (*a_drain)())
	{
		laneDrain.store(a_drain, std::memory_order_release);
		WakePipelineLane();
	}

	void WakePipelineLane()
	{
		(void)Lane().Notify();
	}
}

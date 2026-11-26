// The build pump and the pipeline lane.

#include "Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.h"

#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "RenderGraph/Async/Lanes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include <ORGModuleServices/Async/InboxQueue.h>
#include <ORGModuleServices/Async/SerializedTaskPump.h>

namespace DCLF
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// Set while the startup pipeline step runs, when nothing else needs the preparation workers.
		std::atomic<bool> buildFullWidth{ false };

		/**
		 * @brief A lock-free, width-limited pump onto the preparation pool. A runner gives its slot back only once the count reads zero,
		 * then looks once more, so a push that found every slot taken is never left without a runner.
		 *
		 * Shader stages run before queued pipeline builds. A stage is mostly a disk cache hit of a few milliseconds, and a pipeline
		 * build can take a hundred. A stage behind a load's builds held its pipeline, and the objects drawn with it, for seconds.
		 */
		struct BuildPump
		{
			using Task = std::function<void()>;
			struct Item
			{
				Task task;
				Clock::time_point queued;
			};

			std::array<org::async::InboxQueue<Item>, 2> tasks;  // BuildKind order: stages first; runners never spin on each other
			// A pop may precede its push's count, so this can read below zero for a moment.
			std::atomic<std::int64_t> queued{ 0 };
			std::atomic<unsigned> runners{ 0 };

			/** @brief Runners allowed at once: BuildWidth(), or every preparation worker during the startup pipeline step. */
			static unsigned Width()
			{
				return buildFullWidth.load(std::memory_order_relaxed) ? (std::max)(1u, ::RenderGraph::Frame::PreparationWorkers()) : BuildWidth();
			}

			// CS_DCLF_STATS: each kind's queue waits and run times in ms, and the deepest queue, under statsLock.
			std::mutex statsLock;
			std::array<std::vector<float>, 2> waits, runs;
			std::int64_t deepest = 0;

			bool Submit(BuildKind a_kind, Task a_task)
			{
				tasks[static_cast<std::size_t>(a_kind)].Push({ std::move(a_task), Clock::now() });
				const std::int64_t depth = queued.fetch_add(1) + 1;
				if (Diagnostics::Observe().stats) {
					std::scoped_lock lock(statsLock);
					deepest = (std::max)(deepest, depth);
				}
				if (TakeRunner())
					Dispatch();
				return true;
			}

			bool TakeRunner()
			{
				for (unsigned running = runners.load(); running < Width();)
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
				Item item;
				std::size_t kind = 0;
				while (kind < tasks.size() && !tasks[kind].TryPop(item))
					++kind;
				if (kind < tasks.size()) {
					queued.fetch_sub(1);
					const auto start = Clock::now();
					// ORG's tasks report failures through their artifacts. Nothing may stop the pump.
					try {
						item.task();
					} catch (...) {
					}
					if (Diagnostics::Observe().stats) {
						const auto ms = [](Clock::duration a_span) { return std::chrono::duration<float, std::milli>(a_span).count(); };
						const auto end = Clock::now();
						std::scoped_lock lock(statsLock);
						waits[kind].push_back(ms(start - item.queued));
						runs[kind].push_back(ms(end - start));
					}
				}
				// A runner past the width, after the startup step narrowed it, gives its slot back. The others keep running.
				if (queued.load() > 0 && runners.load() <= Width()) {
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

	void SetBuildFullWidth(bool a_full)
	{
		buildFullWidth.store(a_full, std::memory_order_relaxed);
	}

	std::function<bool(std::function<void()>)> BuildSubmitter(BuildKind a_kind)
	{
		return [a_kind](std::function<void()> a_task) { return Pump().Submit(a_kind, std::move(a_task)); };
	}

	BuildPumpStats TakeBuildPumpStats()
	{
		auto& pump = Pump();
		BuildPumpStats out;
		std::scoped_lock lock(pump.statsLock);
		const auto summarize = [](std::vector<float>& a_ms, BuildPumpStats::Spread& a_out) {
			if (a_ms.empty())
				return;
			std::sort(a_ms.begin(), a_ms.end());
			a_out.p50 = a_ms[a_ms.size() / 2];
			a_out.p95 = a_ms[(std::min)(a_ms.size() - 1, a_ms.size() * 95 / 100)];
			a_out.max = a_ms.back();
			for (const float ms : a_ms)
				a_out.total += ms;
			a_ms.clear();
		};
		for (std::size_t k = 0; k < out.kinds.size(); ++k) {
			out.kinds[k].tasks = static_cast<std::uint32_t>(pump.runs[k].size());
			summarize(pump.waits[k], out.kinds[k].wait);
			summarize(pump.runs[k], out.kinds[k].run);
		}
		out.deepest = static_cast<std::uint32_t>((std::max<std::int64_t>)(std::exchange(pump.deepest, 0), 0));
		out.width = BuildPump::Width();
		return out;
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

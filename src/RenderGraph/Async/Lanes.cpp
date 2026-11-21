// Lanes: CS's shared executor, its threads' names and priorities, and the lanes that run on it.
#include "RenderGraph/Async/Lanes.h"

#include "RenderGraph/Fatal.h"

#include <ORGModuleServices/Async/ExecutorTaskService.h>
#include <Render/Runtime/ITaskService.h>
#include <Windows.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <thread>

namespace RenderGraph::Frame
{
	namespace
	{
		// The domain of the executor thread this is, or none.
		constexpr std::uint32_t kNotExecutor = ~0u;
		thread_local std::uint32_t t_executorDomain = kNotExecutor;

		// The coordinator, serial and frame-critical threads run above the engine's job threads.
		constexpr int kRaised = THREAD_PRIORITY_ABOVE_NORMAL;

		// The blocking threads: texture streaming's window reads and program builds.
		constexpr unsigned kStreamingWorkers = 2;

		// A worker running an above-normal caller's ParallelFor chunk is raised to its priority, so the engine's job threads cannot preempt it.
		// A frame-critical job runs at the coordinator's priority.
		Executor::ThreadPriority WorkerBoost()
		{
			return { [] { return GetThreadPriority(GetCurrentThread()); }, [](int a_priority) { SetThreadPriority(GetCurrentThread(), a_priority); }, kRaised };
		}

		void NameThread(const std::wstring& a_name) { SetThreadDescription(GetCurrentThread(), a_name.c_str()); }

		void StartThread(org::async::TaskClass a_class, unsigned a_index)
		{
			t_executorDomain = a_class.domain;
			const int raised = kRaised;
			if (a_class.domain == Executor::Scene.domain) {
				// Serial work such as a scene pass. The render thread waits for it only at Present.
				NameThread(L"CS serial lane");
				SetThreadPriority(GetCurrentThread(), raised);
				return;
			}
			if (a_class.domain == Executor::Coordinator.domain) {
				// The engine's job threads run at normal priority and the render thread joins the frame's jobs, so the coordinator goes above them.
				NameThread(L"CS coordinator");
				SetThreadPriority(GetCurrentThread(), raised);
				return;
			}
			if (a_class.domain == Executor::FrameCriticalThread.domain) {
				// Waits at frame-critical priority, so a frame-critical job starts even while the engine's threads hold every core.
				NameThread(L"CS frame worker " + std::to_wstring(a_index));
				SetThreadPriority(GetCurrentThread(), raised);
				return;
			}
			if (a_class.domain == Executor::Streaming.domain) {
				NameThread(L"CS blocking worker " + std::to_wstring(a_index));
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
				return;
			}
			NameThread(L"CS worker " + std::to_wstring(a_index));
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		}
	}

	unsigned PreparationWorkers()
	{
		static const unsigned workers = [] {
			const unsigned hardware = std::thread::hardware_concurrency();
			return hardware > 3 ? hardware - 2 : 1u;
		}();
		return workers;
	}

	unsigned FrameCriticalWorkers()
	{
		static const unsigned workers = (std::max)(2u, PreparationWorkers() / 4);
		return workers;
	}

	Executor& SharedExecutor()
	{
		// Never destroyed, because joining threads in a DLL's static destruction, under the loader lock after ExitProcess ended them, is unreliable.
		static auto* executor = [] {
			auto desc = Executor::DefaultDesc(PreparationWorkers(), kStreamingWorkers);
			desc.onThreadStart = StartThread;
			desc.priority = WorkerBoost();
			desc.frameCriticalWorkers = FrameCriticalWorkers();
			return new Executor(std::move(desc));
		}();
		return *executor;
	}

	bool OnExecutorThread()
	{
		return t_executorDomain != kNotExecutor;
	}

	bool OnExecutorThread(org::async::TaskClass a_class)
	{
		return t_executorDomain == a_class.domain;
	}

	std::shared_ptr<org::runtime::ITaskService> OrgTaskService()
	{
		// Never destroyed, as the executor it runs on.
		// A task that throws is fatal. ORG's tasks never go to the blocking threads: their jobs may wait on the graph's locks (a texture
		// window's immediate submission waits for the sequencer), and an ORG task behind them, such as a command-list pool's reset that a
		// submission waits for while it holds the sequencer, deadlocked the render thread (2026-11-21).
		static auto* service = new std::shared_ptr<org::runtime::ITaskService>(org::async::CreateExecutorTaskService(SharedExecutor(),
			{ [](std::string_view a_error) { RenderGraph::Fatal("ORG task", a_error); } }));
		return *service;
	}

	LaneDesc FatalOnRefusal(LaneDesc a_desc)
	{
		if (!a_desc.refused)
			a_desc.refused = [name = a_desc.name] {
				RenderGraph::Fatal("lanes", fmt::format("the {} lane stopped: its task was refused or its work threw", name));
			};
		return a_desc;
	}

	org::async::SerializedTaskPump& MakeLane(LaneDesc a_desc, std::function<void()> a_drain)
	{
		// Never destroyed: a completion may still wake it while the process tears down.
		return org::async::MakeLane(SharedExecutor(), FatalOnRefusal(std::move(a_desc)), std::move(a_drain));
	}
}


// SceneScheduler: DCLF's executor, scopes, state graph and lane pumps, and thread names and priorities. Created on first use, never destroyed.
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"

#include <ORGModuleServices/Async/SerializedTaskPump.h>

#include <Windows.h>

#include <thread>

namespace DCLF
{
	unsigned SceneScheduler::PreparationWorkers()
	{
		static const unsigned workers = [] {
			const auto value = SwitchValue(Switch::Workers);
			if (const unsigned asked = value.empty() ? 0u : static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10)))
				return asked;
			const unsigned hardware = std::thread::hardware_concurrency();
			return hardware > 3 ? hardware - 2 : 1u;
		}();
		return workers;
	}

	unsigned SceneScheduler::FrameCriticalWorkers()
	{
		static const unsigned workers = [] {
			if (const auto value = SwitchValue(Switch::FrameWorkers); !value.empty())
				return static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
			return std::max(2u, PreparationWorkers() / 4);
		}();
		return workers;
	}

	namespace
	{
		// A worker running an above-normal caller's ParallelFor chunk is raised to its priority, so the engine's job threads cannot preempt it.
		// A frame-critical job runs at the coordinator's priority. CS_DCLF_ASYNC_PRIORITY=noboost turns this off for A/B tests.
		SceneExecutor::ThreadPriority WorkerBoost()
		{
			if (SwitchValue(Switch::AsyncPriority) == "noboost")
				return {};
			return { [] { return GetThreadPriority(GetCurrentThread()); }, [](int a_priority) { SetThreadPriority(GetCurrentThread(), a_priority); },
				SwitchValue(Switch::AsyncPriority) == "normal" ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_ABOVE_NORMAL };
		}
	}

	SceneExecutor& SceneScheduler::Executor()
	{
		// Never destroyed, because joining threads in a DLL's static destruction, under the loader lock after ExitProcess ended them, is unreliable.
		static auto* executor = new SceneExecutor(PreparationWorkers(), [](org::async::TaskClass a_class, unsigned a_index) {
			// Marks DCLF's threads for the release guard, CS_DCLF_RELEASE_GUARD.
			ReleaseGuard::MarkThread(a_class.domain == SceneExecutor::Scene.domain ? 1 : 2);
			if (a_class.domain == SceneExecutor::Scene.domain) {
				SetThreadDescription(GetCurrentThread(), L"CS DCLF scene");
				// The render thread only waits for it at Present. It runs above the engine's job threads, like the coordinator.
				if (SwitchValue(Switch::AsyncPriority) != "normal")
					SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
				return;
			}
			if (a_class.domain == SceneExecutor::Coordinator.domain) {
				SetThreadDescription(GetCurrentThread(), L"CS DCLF coordinator");
				// The engine's job threads run at normal priority, and the render thread joins the frame's jobs, so the coordinator goes above them unless asked not to.
				if (SwitchValue(Switch::AsyncPriority) != "normal")
					SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
				return;
			}
			if (a_class.domain == SceneExecutor::FrameCriticalThread.domain) {
				// Waiting at frame-critical priority, so a frame-critical job starts even while the engine's threads hold every core.
				SetThreadDescription(GetCurrentThread(), (L"CS DCLF frame worker " + std::to_wstring(a_index)).c_str());
				SetThreadPriority(GetCurrentThread(), SwitchValue(Switch::AsyncPriority) == "normal" ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_ABOVE_NORMAL);
				return;
			}
			if (a_class.domain == SceneExecutor::Streaming.domain) {
				SetThreadDescription(GetCurrentThread(), (L"CS DCLF texture loader " + std::to_wstring(a_index)).c_str());
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
				return;
			}
			SetThreadDescription(GetCurrentThread(), (L"CS DCLF worker " + std::to_wstring(a_index)).c_str());
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		}, WorkerBoost(), FrameCriticalWorkers());
		return *executor;
	}

	const org::async::Scope& SceneScheduler::SceneLaneScope()
	{
		static const org::async::Scope scope = Executor().CreateScope("DCLF scene");
		return scope;
	}

	Runtime::SceneGraph& SceneScheduler::Graph()
	{
		// Never destroyed, like the executor it runs on. The graph holds it without owning it.
		static auto* graph = new Runtime::SceneGraph(std::shared_ptr<org::async::GraphScheduler>(&Executor(), [](org::async::GraphScheduler*) {}));
		return *graph;
	}

	org::async::SerializedTaskPump* SceneScheduler::MakeLanePump(SceneExecutor& a_executor, const org::async::Scope& a_scope, org::async::TaskClass a_class,
		org::async::TaskDispatch a_dispatch, std::string_view a_name, std::function<void()> a_drain, std::function<void()> a_refused)
	{
		auto* pump = new org::async::SerializedTaskPump;
		pump->Configure(
			[&a_executor, &a_scope, a_class, a_dispatch, a_name](org::async::SerializedTaskPump::Task a_task) {
				return a_executor.Dispatch(a_scope, a_class, a_dispatch, a_name, [task = std::move(a_task)](const org::async::TaskContext&) { task(); });
			},
			std::move(a_drain), std::move(a_refused));
		return pump;
	}

	const org::async::Scope& SceneScheduler::Scope()
	{
		static const org::async::Scope scope = Executor().CreateScope("DCLF");
		return scope;
	}
}

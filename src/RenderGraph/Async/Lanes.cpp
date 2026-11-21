// Lanes: CS's shared executor, its threads' names and priorities, and the lanes that run on it.
#include "RenderGraph/Async/Lanes.h"

#include "RenderGraph/Fatal.h"

#include <Render/Runtime/ITaskService.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <string>
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
		static auto* executor = new Executor(PreparationWorkers(), StartThread, WorkerBoost(), FrameCriticalWorkers());
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

	namespace
	{
		// One of ORG's scopes. It counts its tasks, queued or running. Cancel drops those not yet started, and Wait returns once
		// every one has left. A task leaves when the executor drops it, run or not.
		class OrgScope final : public org::runtime::ITaskScope
		{
		public:
			void Cancel() noexcept override { cancelled.store(true, std::memory_order_release); }
			void Wait() override
			{
				for (auto value = pending.load(std::memory_order_acquire); value != 0; value = pending.load(std::memory_order_acquire))
					pending.wait(value);
			}
			void CancelAndWait() override
			{
				Cancel();
				Wait();
			}
			bool Cancelled() const noexcept { return cancelled.load(std::memory_order_acquire); }
			void Enter() { pending.fetch_add(1, std::memory_order_acq_rel); }
			void Leave()
			{
				if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
					pending.notify_all();
			}

		private:
			std::atomic_bool cancelled{ false };
			std::atomic<std::size_t> pending{ 0 };
		};

		// A task's hold on its scope, released when the last copy of the task goes.
		struct ScopeTicket
		{
			std::shared_ptr<OrgScope> scope;
			~ScopeTicket()
			{
				if (scope)
					scope->Leave();
			}
		};

		class ExecutorTaskService final : public org::runtime::ITaskService
		{
		public:
			ExecutorTaskService() :
				executorScope(SharedExecutor().CreateScope("ORG")) {}

			void ParallelFor(std::string_view a_name, std::size_t a_count, std::function<void(std::size_t)> a_body) override
			{
				ParallelForLimited(a_name, a_count, a_count, std::move(a_body));
			}

			void ParallelForLimited(std::string_view a_name, std::size_t a_count, std::size_t a_concurrency, std::function<void(std::size_t)> a_body) override
			{
				if (!a_count)
					return;
				// At most a_concurrency chunks, so at most that many run at once. The caller takes part.
				const std::size_t chunks = std::clamp<std::size_t>(a_concurrency, 1, a_count);
				SharedExecutor().ParallelFor(a_name, a_count, (a_count + chunks - 1) / chunks, [&a_body](std::size_t a_begin, std::size_t a_end) {
					for (std::size_t i = a_begin; i < a_end; ++i)
						a_body(i);
				});
			}

			std::shared_ptr<org::runtime::ITaskScope> CreateScope(std::string_view) override { return std::make_shared<OrgScope>(); }

			bool Submit(const std::shared_ptr<org::runtime::ITaskScope>& a_scope, org::runtime::TaskPriority a_priority, std::string_view a_name,
				std::function<void()>&& a_task) override
			{
				return Enqueue(a_scope, a_priority, {}, a_name, std::move(a_task));
			}

			bool ScheduleAfter(const std::shared_ptr<org::runtime::ITaskScope>& a_scope, std::chrono::steady_clock::duration a_delay,
				org::runtime::TaskPriority a_priority, std::string_view a_name, std::function<void()>&& a_task) override
			{
				return Enqueue(a_scope, a_priority, a_delay, a_name, std::move(a_task));
			}

		private:
			bool Enqueue(const std::shared_ptr<org::runtime::ITaskScope>& a_scope, org::runtime::TaskPriority a_priority, std::chrono::steady_clock::duration a_delay,
				std::string_view a_name, std::function<void()>&& a_task)
			{
				auto scope = std::dynamic_pointer_cast<OrgScope>(a_scope);
				if ((a_scope && !scope) || (scope && scope->Cancelled()) || !a_task)
					return false;
				if (scope)
					scope->Enter();
				auto ticket = std::make_shared<ScopeTicket>();
				ticket->scope = std::move(scope);
				Executor::Task task = [ticket, work = std::move(a_task), name = std::string(a_name)](const org::async::TaskContext&) mutable {
					if (!ticket->scope || !ticket->scope->Cancelled()) {
						try {
							work();
						} catch (const std::exception& e) {
							RenderGraph::Fatal("ORG task", fmt::format("{} threw: {}", name, e.what()));
						} catch (...) {
							RenderGraph::Fatal("ORG task", fmt::format("{} threw", name));
						}
					}
					// The work's captures go before its scope is left.
					work = nullptr;
					ticket.reset();
				};
				auto& executor = SharedExecutor();
				const bool critical = a_priority == org::runtime::TaskPriority::FrameCritical;
				// Never the streaming lane: its jobs may wait on the graph's locks (a texture window's immediate submission waits for the
				// sequencer), and an ORG task behind them, such as a command-list pool's reset that a submission waits for while it holds
				// the sequencer, deadlocked the render thread (2026-11-21).
				const auto taskClass = Executor::Preparation;
				if (critical && a_delay <= std::chrono::steady_clock::duration::zero())
					return executor.DispatchFrameCritical(executorScope, a_name, std::move(task));
				return executor.DispatchAfter(executorScope, (std::max)(a_delay, std::chrono::steady_clock::duration::zero()), taskClass, a_name, std::move(task));
			}

			org::async::Scope executorScope;  // every ORG task's, never waited on
		};
	}

	std::shared_ptr<org::runtime::ITaskService> OrgTaskService()
	{
		// Never destroyed, as the executor it runs on.
		static auto* service = new std::shared_ptr<org::runtime::ITaskService>(std::make_shared<ExecutorTaskService>());
		return *service;
	}

	Lane::Lane(LaneDesc a_desc, std::function<void()> a_drain) :
		desc(std::move(a_desc)), scope(SharedExecutor().CreateScope(desc.name))
	{
		pump.Configure(
			[this](org::async::SerializedTaskPump::Task a_task) {
				return SharedExecutor().Dispatch(scope, desc.taskClass, desc.dispatch, desc.name,
					[task = std::move(a_task)](const org::async::TaskContext&) { task(); });
			},
			std::move(a_drain),
			[this] {
				if (stopped.load(std::memory_order_acquire))
					return;
				if (desc.refused)
					desc.refused();
				else
					RenderGraph::Fatal("lanes", fmt::format("the {} lane stopped: its task was refused or its work threw", desc.name));
			});
	}

	Lane::~Lane()
	{
		Stop();
	}

	void Lane::Stop()
	{
		if (stopped.exchange(true, std::memory_order_acq_rel))
			return;
		pump.Stop();
		scope->CancelAndWait();
	}

	org::async::SerializedTaskPump& MakeLane(LaneDesc a_desc, std::function<void()> a_drain)
	{
		// Never destroyed: a completion may still wake it while the process tears down.
		auto* lane = new Lane(std::move(a_desc), std::move(a_drain));
		return lane->Pump();
	}
}


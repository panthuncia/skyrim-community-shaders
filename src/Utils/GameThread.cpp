#include "Utils/GameThread.h"

#include "RenderGraph/Engine/MainTick.h"
#include "RenderGraph/Fatal.h"
#include "Snapshot/EngineReads.h"
#include "Testing/Testing.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace GameThread
{
	namespace
	{
		struct Task
		{
			std::string what;
			std::function<void()> run;
		};

		struct Queue
		{
			std::mutex mutex;
			std::vector<Task> tasks;
			std::set<std::string, std::less<>> missed;  // NoteEngineWrite's names, logged once each
		};

		Queue& Get()
		{
			static Queue s_queue;
			return s_queue;
		}

		std::atomic<bool> g_installed{ false };
		std::atomic<std::uint32_t> g_mainThread{ 0 };
		std::atomic<bool> g_inline{ false };  // VR: no main tick, so tasks run where posted
		std::atomic<std::uint64_t> g_missed{ 0 };
		thread_local std::uint32_t t_taskDepth = 0;

		void RunTask(Task& a_task)
		{
			++t_taskDepth;
			a_task.run();
			--t_taskDepth;
		}

		/** @brief The main tick: runs the tasks posted so far, in order. */
		void RunTasks()
		{
			std::vector<Task> tasks;
			{
				auto& queue = Get();
				std::scoped_lock lock(queue.mutex);
				tasks.swap(queue.tasks);
			}
			for (auto& task : tasks)
				RunTask(task);
		}
	}

	void Install()
	{
		if (g_installed.exchange(true))
			return;
		g_mainThread.store(::GetCurrentThreadId(), std::memory_order_release);
		if (REL::Module::IsVR()) {
			g_inline.store(true, std::memory_order_release);
			logger::info("[GameThread] VR has no main tick: game-thread tasks run where they are posted");
			RunTasks();
			return;
		}
		if (!RenderGraph::Engine::MainTick::Add("the game-thread command queue", RunTasks))
			RenderGraph::Fatal("game-thread queue", "the frame-job wait is not hooked, so the UI's engine writes would never run");
		::Testing::RegisterExitReport([] { ::Testing::SetReport("ui_engine_writes", g_missed.load(std::memory_order_relaxed)); });
	}

	void Post(std::string_view a_what, std::function<void()> a_task)
	{
		if (g_inline.load(std::memory_order_acquire)) {
			Task task{ std::string(a_what), std::move(a_task) };
			RunTask(task);
			return;
		}
		auto& queue = Get();
		std::scoped_lock lock(queue.mutex);
		queue.tasks.push_back({ std::string(a_what), std::move(a_task) });
	}

	bool InTask() noexcept
	{
		return t_taskDepth != 0;
	}

	bool CanWriteNow() noexcept
	{
		return InTask() || (::GetCurrentThreadId() == g_mainThread.load(std::memory_order_acquire) && !InUiFrame());
	}

	void Run(std::string_view a_what, std::function<void()> a_task)
	{
		if (CanWriteNow())
			a_task();
		else
			Post(a_what, std::move(a_task));
	}

	UiFrameScope::UiFrameScope() noexcept
	{
		Snapshot::EngineReads::EnterUiFrame();
	}

	UiFrameScope::~UiFrameScope()
	{
		Snapshot::EngineReads::LeaveUiFrame();
	}

	bool InUiFrame() noexcept
	{
		return Snapshot::EngineReads::InUiFrame();
	}

	void NoteEngineWrite(std::string_view a_what)
	{
		if (!InUiFrame() || InTask())
			return;
		// The test runner's commands run at Present, outside the ImGui frame: not counted.
		g_missed.fetch_add(1, std::memory_order_relaxed);
		{
			auto& queue = Get();
			std::scoped_lock lock(queue.mutex);
			if (!queue.missed.emplace(a_what).second)
				return;
		}
		logger::error("[GameThread] an engine write from the ImGui frame outside the game-thread queue: {}", a_what);
		if (::Testing::Configured())
			RenderGraph::Fatal("game-thread queue", std::format("the ImGui frame wrote engine state directly: {}", a_what));
	}

	std::uint64_t MissedWrites() noexcept
	{
		return g_missed.load(std::memory_order_relaxed);
	}
}

#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

/**
* @brief The game-thread command queue. The ImGui frame (and other render-side code) writes engine and game state only through it:
* it posts a task, and the task runs on the main thread at the next main tick (RenderGraph::Engine::MainTick, after the frame-job
* wait in Main::Update), where game logic runs. The ImGui frame runs at the render frame's Present, on the thread that renders.
* For guest and for updates the game thread renders, that is the main thread, so a posted write lands at the end of the same
* Main::Update, before the next sky and game update. For a routed native update it is the render thread, and the write lands at
* the game thread's next main tick.
*/
namespace GameThread
{
	/** @brief Main thread, at plugin load. Hooks the main tick; fatal when it cannot. On VR (no main tick) tasks run where posted. */
	void Install();

	/**
	* @brief Any thread. Queues a_task to run on the main thread at the next main tick, in post order. A task that posts more leaves
	* them for the next tick. a_what names the write for the logs.
	*/
	void Post(std::string_view a_what, std::function<void()> a_task);

	/** @brief Whether this thread is running a posted task now. */
	bool InTask() noexcept;

	/** @brief Whether game state may be written here and now: inside a posted task, or on the main thread outside the ImGui frame. */
	bool CanWriteNow() noexcept;

	/** @brief Runs a_task now where CanWriteNow holds (game hooks, engine events), else posts it. */
	void Run(std::string_view a_what, std::function<void()> a_task);

	/**
	* @brief Marks the ImGui frame on this thread. While one is open, engine reads count as UI-frame reads in the live-read detector,
	* and NoteEngineWrite catches writes made outside a posted task. Nests.
	*/
	class UiFrameScope
	{
	public:
		UiFrameScope() noexcept;
		~UiFrameScope();
		UiFrameScope(const UiFrameScope&) = delete;
		UiFrameScope& operator=(const UiFrameScope&) = delete;
	};

	/** @brief Whether this thread is inside a UiFrameScope. */
	bool InUiFrame() noexcept;

	/**
	* @brief Called by code that writes engine or game state. Inside the ImGui frame and outside a posted task it is a missed
	* conversion: logged once per a_what, counted in the test report, and fatal in test runs.
	*/
	void NoteEngineWrite(std::string_view a_what);

	/** @brief How many writes NoteEngineWrite caught so far. */
	std::uint64_t MissedWrites() noexcept;
}

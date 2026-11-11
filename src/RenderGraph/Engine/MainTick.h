#pragma once

#include <functional>
#include <string_view>

/**
 * @brief The main thread's per-frame point after the frame's jobs: JobListManager's frame-job wait near the end of Main::Update
 * (SE 34554, AE 35409; Main::Update is its only caller in both). When it returns, the frame's jobs are done (the AI job and its
 * queued 3D attaches included) and the engine is back to one thread. One detour serves every user.
 *
 * SKSE's AddTask runs on BSJobs job threads in this setup, not the main thread: this is the main-thread point to use instead (VRAM
 * Budget's controller, the test harness's Bink requests, the screen blood client's setup).
 */
namespace RenderGraph::Engine::MainTick
{
	/**
	 * @brief Adds a_callback, run on the main thread after every frame-job wait, in the order added. The first Add installs the
	 * hook. Main thread, at plugin load or a feature's PostPostLoad; a callback must not call Add. False, logged, when the hook could
	 * not be installed: the callback never runs.
	 */
	bool Add(std::string_view a_name, std::function<void()> a_callback);
}

#pragma once

#include <functional>
#include <string_view>

/**
* @brief A main-thread point once per frame, after JobListManager's frame-job wait in Main::Update (SE 34554, AE 35409).
* When it returns the frame's jobs are done and the engine is back to one thread.
* SKSE's AddTask runs on job threads in this setup, so use this when code must run on the main thread.
*/
namespace RenderGraph::Engine::MainTick
{
	/**
	* @brief Runs a_callback on the main thread after every frame-job wait, in the order added. Main thread, at plugin load.
	* A callback must not call Add. False, logged, when the hook could not be installed.
	*/
	bool Add(std::string_view a_name, std::function<void()> a_callback);
}

#pragma once

#include <cstdint>

/**
* @brief The loading screen and the startup logo on the render graph, in the graph modes.
*
* The engine renders each frame inside Renderer::Lock on one thread at a time: the main thread, the loading loop's serving
* thread, or the MoviePlayer thread during the startup logo. Whichever thread renders owns the graph's frame work. The one
* claim point is Renderer::Begin, which the frame driver detours (Frame/FrameDriver.cpp). Frame revisions adopt on the main
* thread only (D-4), so the other threads' frames run the revision the main thread adopted last.
*/
namespace RenderGraph::Engine::LoadingScreen
{
	/** @brief Main thread, at plugin load. Detours SetLoadingScreenActive in every mode. */
	void Install();

	/**
	* @brief How many SetLoadingScreenActive calls have returned, true or false. Any thread. Every load makes at least one: a coc
	* nearby makes only the false call, with no loading menu.
	*/
	std::uint64_t Calls();
}

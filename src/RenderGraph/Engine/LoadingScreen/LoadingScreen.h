#pragma once

/**
* @brief The loading screen and the startup logo on the render graph, with CS_ORG_LOADING_THREAD=1.
*
* The engine renders each frame inside Renderer::Lock on one thread at a time: the main thread, the loading loop's serving
* thread, or the MoviePlayer thread during the startup logo. With the switch on, whichever thread renders owns the graph's
* frame work. The one claim point is Renderer::Begin.
*/
namespace RenderGraph::Engine::LoadingScreen
{
	/** @brief Not on VR, whose loop was not checked. Any thread. */
	bool Requested();

	/** @brief Main thread, at plugin load. Detours Renderer::Begin and SetLoadingScreenActive when requested. */
	void Install();
}

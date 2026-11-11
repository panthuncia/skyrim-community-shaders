#pragma once

/**
 * @brief The loading screen and the startup logo on the render graph (org-full-port.md P5.12), behind CS_ORG_LOADING_THREAD=1.
 * docs/development/p5/loading-screen.md has the RE and the design.
 *
 * The engine renders every frame inside Renderer::Lock, on one thread at a time: the main thread, the loading loop on
 * JobListManager's ServingThread (SE 38085, AE 39039) during a load, or the MoviePlayer thread during the startup logo. With the
 * switch on, the thread that renders owns the render graph's frame work (FrameAnchors::IsRenderOwner). The one claim point is
 * Renderer::Begin, which every one of those frames calls first (FrameAnchors::ClaimRenderOwnership).
 */
namespace RenderGraph::Engine::LoadingScreen
{
	/** @brief Whether CS_ORG_LOADING_THREAD=1 asked for the render-owner rule (not on VR, whose loop was not checked). Any thread. */
	bool Requested();

	/**
	 * @brief Main thread, at plugin load (RenderGraph::Engine::InstallGameHooks): detours Renderer::Begin (SE 75460, AE 77245), the
	 * claim point, and ServingThread::SetLoadingScreenActive (SE 34552, AE 35407), which logs the load's ownership changes, when
	 * requested. Nothing otherwise.
	 */
	void Install();
}

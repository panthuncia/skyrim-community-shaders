#pragma once

/**
 * @brief The engine's frame skeleton on the render graph (org-full-port.md P5.1; RE in docs/development/p5/skeleton.md).
 *
 * With CS_ORG_ENGINE_SKELETON=1 the engine's own clears and its main depth copy become graph passes:
 * - Every ClearRenderTargetView and ClearDepthStencilView the engine issues from Renderer::ApplyState (75462 / 77247),
 *   BSGraphics::SetDirtyStates (75580 / 77386), Renderer::ClearColor (75465 / 77251) and Renderer::ClearDepthStencil
 *   (75466 / 77252). That covers the shadow map slices (BSShadowLight::RenderShadowmap), the reflection faces
 *   (BSCubeMapCamera::RenderFaces), the main depth at Main::RenderDepth's start, Main::Draw's normal and kMAIN clears, the
 *   G-buffer clears Deferred asks for, and FinishAccumulating's stencil clear.
 * - The copy of the main depth into kPOST_ZPREPASS_COPY at Main::RenderDepth's end (100421 / 107139), in FinishAccumulating
 *   (99938 / 106583), and CS's own in Deferred::DeferredPasses.
 *
 * Each clear is a draw through a run gate on one target view (a slice or a face), so its recording is kept and a run does only
 * the clears the engine asked for. The engine's call is withheld when the graph pass ran. The binds stay with D3D11: draws
 * DCLF does not claim yet still read them.
 *
 * Fallback, per call: a view outside the engine's tables, a format the clear programs cannot draw (integer formats), a clear
 * value other than the engine's (depth 1, stencil 0), a call off the main thread (a loading screen), a target seen for the
 * first time, or an anchor run that did not submit the pass. Each of those gets the engine's own D3D11 call.
 *
 * Default off: nothing is installed, and the engine's calls are unchanged.
 */
namespace RenderGraph::Engine::Skeleton
{
	/** @brief Whether CS_ORG_ENGINE_SKELETON=1 is set. */
	bool Requested();

	/** @brief With the other engine hooks (Hooks.cpp): the call-site patches, each checked against the bytes the RE found. */
	void Install();

	/** @brief State::Setup, before the render graph initializes: the skeleton's anchors are reserved. */
	void Reserve();

	/** @brief State::Setup, once the graph is up and the engine's targets moved: the run gates. Clients register as targets appear. */
	void Setup();

	/**
	 * @brief Deferred::DeferredPasses: the main depth copied into kPOST_ZPREPASS_COPY on the graph, here in the stream.
	 * @return Whether the copy ran; false: the caller copies with D3D11.
	 */
	bool CopyPostZPrepassDepth();
}

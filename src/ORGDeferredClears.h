#pragma once

/**
 * @brief The GBuffer clears of Deferred::StartDeferred as a render-graph pass, in guest and native mode alike (on whenever the
 * render graph runs; CS_ORG_DEFERRED_CLEARS=0 keeps the engine's clears).
 *
 * StartDeferred binds CS's GBuffer targets (NormalRoughness, Albedo, Specular, Reflectance, Masks, Masks2) with SRTM_CLEAR, so the
 * engine's next state flush after the Prepass anchor clears each one through D3D11 (ClearRenderTargetView) with the renderer's clear
 * colour. With this path on, the clears are one render pass (each target's load op a clear) that runs last at the Prepass anchor,
 * where the targets are declared render targets, and StartDeferred binds them with SRTM_NO_CLEAR instead. The clear colour is the
 * renderer's at the anchor's commit; a new colour records the pass again (its revision), no graph build.
 *
 * The targets are engine targets imported at each build of the anchor's graph (FrameAnchors::EngineTarget: the published ones when
 * ORG owns the engine's targets), so a resize is picked up by the build its new imports ask for. When the pass is not in the anchor's
 * built graph (before it joins, or after a failure), StartDeferred keeps the engine's clears.
 *
 * Render thread. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
namespace ORGDeferredClears
{
	/** @brief Whether the path is wanted: not CS_ORG_DEFERRED_CLEARS=0. */
	bool Requested();

	/** @brief Before the render graph initializes: reserves the Prepass anchor when requested. */
	void Reserve();

	/** @brief After the render graph initializes and the GBuffer targets exist (Deferred::SetupResources): registers the client. */
	void Setup();

	/**
	 * @brief StartDeferred, before the Prepass anchor runs: whether this run of the anchor clears the GBuffer targets on the graph.
	 * True: bind them with SRTM_NO_CLEAR. False: keep the engine's SRTM_CLEAR.
	 */
	bool TakeClears();
}

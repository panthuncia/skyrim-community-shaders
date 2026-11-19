#pragma once

/**
* @brief The engine's own clears and main depth copies as graph passes, with CS_ORG_ENGINE_SKELETON=1 (default off).
*
* Each view the engine clears is one pass, registered once at Setup. Clears are draws through a run gate and colours are
* latched, so the recording is kept and each run does only the clears asked for. A group registers again only after a resize.
* Unsupported cases fall back to the engine's D3D11 call: unknown views, integer formats, clear values other than depth 1
* and stencil 0, a non-owner thread, or a registration not built yet.
*
* In native mode a world frame's clear runs inside the graph of the epoch that writes the target first after the call. The
* engine's call only records it on the CPU. Clears with nowhere to run are mismatches, and some with no reader are dropped.
*/
namespace RenderGraph::Engine::Skeleton
{
	bool Requested();

	/** @brief Each call-site patch is checked against the bytes the RE found. */
	void Install();

	/** @brief State::Setup, before the render graph initializes. Reserves the anchors. */
	void Reserve();

	/** @brief State::Setup, once the graph is up and the engine's targets moved. */
	void Setup();

	/** @brief At each Renderer::Begin. A clear or copy still waiting for its host from the last frame is a tap error and is dropped. */
	void BeginFrame();

	/** @brief Deferred::DeferredPasses. False means the caller copies with D3D11. */
	bool CopyPostZPrepassDepth();
}

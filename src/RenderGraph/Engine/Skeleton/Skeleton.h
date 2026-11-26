#pragma once

/**
* @brief The engine's own clears and main depth copies as graph passes, in graph mode (Engine::GraphMode).
*
* Each view the engine clears is one pass of a client declared at Setup (Frame::DeclareClient). Clears are draws through a run
* gate and colours are latched, so the recording is kept and each run does only the clears asked for. A frame start observes
* the engine's targets; a remade one registers its group again with the next revision, which starts once the new builds and
* clear programs are built. A resize is a new version of the same slot and registers nothing. Setup failures are fatal. Per call, unsupported cases still go to the engine's D3D11 call: unknown views, integer formats,
* clear values other than depth 1 and stencil 0, a non-owner thread, or a registration not built yet.
*
* In native mode a world frame's clear runs inside the graph of the epoch that writes the target first after the call. The
* engine's call only records it on the CPU. Clears with nowhere to run are mismatches, and some with no reader are dropped.
*/
namespace RenderGraph::Engine::Skeleton
{
	/** @brief Each call-site patch is checked against the bytes the RE found. */
	void Install();

	/** @brief State::Setup, once the graph is up and the engine's targets moved. */
	void Setup();

	/** @brief At each Renderer::Begin. A clear or copy still waiting for its host from the last frame is a tap error and is dropped. */
	void BeginFrame();

	/** @brief Deferred::DeferredPasses. False means the caller copies with D3D11. */
	bool CopyPostZPrepassDepth();

	// Native's RenderWorld driver: the clears and the copy the engine's resolve functions made, by target, in their place in the
	// frame. Each goes where the engine's call would have gone. False when the skeleton could not take it. Render owner.

	/** @brief A colour target's clear, as a state flush clears a target bound with SRTM_CLEAR. */
	bool ClearColour(RE::RENDER_TARGET a_target, const float a_colour[4]);

	/** @brief A depth target's clear to depth 1 and stencil 0 with D3D11 clear flags a_flags, as Renderer::ClearDepthStencil makes it. */
	bool ClearDepth(RE::RENDER_TARGET_DEPTHSTENCIL a_target, std::uint32_t a_flags);

	/** @brief kMAIN into its copy, as SetRenderTarget's force-copy restore (SRTM_FORCE_COPY_RESTORE) of kMAIN makes it. */
	bool CopyMain();
}

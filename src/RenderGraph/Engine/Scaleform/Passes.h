#pragma once

namespace RenderGraph::Engine::Scaleform
{
	struct Display;
}

/**
 * @brief The render-graph side of Scaleform's ORG path (docs/development/p5/scaleform.md, "The ORG pass"): the Interface anchor's
 * client, its surfaces (each target and depth pair the displays draw onto, with a pass for displays without masks and one with
 * the depth-stencil), the pipelines (a ProgramCache filled at setup), and the per-display commit. Render thread throughout; the passes' Record
 * runs where the graph runs it, reads only the frame a commit stored for its run (RunSlots), and draws through the
 * client's run gate.
 */
namespace RenderGraph::Engine::Scaleform::Passes
{
	/** @brief At load (CS_ORG_SCALEFORM): the Interface anchor reserved. False, logged, when it cannot be. */
	bool Reserve();

	/**
	 * @brief Engine::PostSetup (State::Setup, after the features' SetupResources): the samplers, the run gate, the staging pool, the
	 * framebuffer's and HDR's UI texture's surfaces (each with kMAIN's depth-stencil), every pipeline their passes can draw with queued
	 * on the program build thread, and the client registered; the setup's build point then builds the Interface graph with it. Logged
	 * and off when anything cannot be made.
	 */
	void Setup();

	/** @brief Whether displays are captured for the graph: set up, not failed, the runtime active with the Interface epoch. */
	bool Ready();

	/**
	 * @brief Draws a_display on the graph here, as the Interface anchor's run. False when it did not (the caller replays the
	 * display into the D3D11 renderer), with a_reason: an inherent refusal (a render target other than the framebuffer and HDR's UI
	 * texture, a mask on another depth-stencil, a segment longer than the run gate, the anchor's ring full), a surface made again (a
	 * resize or an HDR change, until the next build point), or a failure (a pipeline that cannot be made, a texture whose view
	 * cannot be resolved, a run that did not execute: logged as an error). A pipeline not built yet is built here. Render thread.
	 */
	bool Submit(Display& a_display, const char*& a_reason);
}

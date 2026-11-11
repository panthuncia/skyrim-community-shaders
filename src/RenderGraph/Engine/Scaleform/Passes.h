#pragma once

namespace RenderGraph::Engine::Scaleform
{
	struct Display;
}

/**
 * @brief The render-graph side of Scaleform's ORG path (docs/development/p5/scaleform.md, "The ORG pass"): the Interface anchor's
 * client, its surfaces (each target and depth pair the displays draw onto, with a pass for displays without masks and one with
 * the depth-stencil), the pipelines (a ProgramCache), and the per-display commit. Render thread throughout; the passes' Record
 * runs where the graph runs it, reads only the frame a commit stored for its frame slot (RunSlots), and draws through the
 * client's run gate.
 */
namespace RenderGraph::Engine::Scaleform::Passes
{
	/** @brief At load (CS_ORG_SCALEFORM): the Interface anchor reserved. False, logged, when it cannot be. */
	bool Reserve();

	/**
	 * @brief Whether a display may be captured for the graph now: the runtime is active with the Interface anchor's epoch, the path
	 * has not failed, and the Interface graph holds the client. The first call sets the path up (samplers, run gate, staging pool,
	 * the framebuffer's and HDR's UI texture's surfaces) and registers the client; false until the graph is built with it at the
	 * next build point. Render thread.
	 */
	bool Ready();

	/**
	 * @brief Draws a_display on the graph here, as the Interface anchor's run. False when it did not (the caller replays the
	 * display into the D3D11 renderer), with a_reason saying why: the surface is new (the graph is being built with it) or cannot
	 * be imported, a pipeline is still building or cannot be made, the segment is longer than the run gate, a texture cannot be
	 * resolved, the anchor cannot run now, or the epoch did not run with the client in it. Render thread, at EndDisplay.
	 */
	bool Submit(Display& a_display, const char*& a_reason);
}

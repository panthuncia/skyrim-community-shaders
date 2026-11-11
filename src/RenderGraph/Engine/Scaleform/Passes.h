#pragma once

namespace RenderGraph::Engine::Scaleform
{
	struct Display;
}

/**
 * @brief The render-graph side of Scaleform's ORG path (docs/development/p5/scaleform.md, "The ORG pass"): the Interface anchor's
 * client, its surfaces (each target and depth pair the displays draw onto, with a pass of its own), the pipelines, and the
 * per-display commit. Render thread throughout; the passes' Record runs where the graph runs it and reads only what a commit
 * stored for its frame slot.
 */
namespace RenderGraph::Engine::Scaleform::Passes
{
	/** @brief At load (CS_ORG_SCALEFORM): the Interface anchor reserved. False, logged, when it cannot be. */
	bool Reserve();

	/**
	 * @brief Whether a display may be captured for the graph now: the runtime is active with the Interface anchor's epoch and the
	 * path has not failed. The first true call makes the samplers. Render thread.
	 */
	bool Ready();

	/**
	 * @brief Draws a_display on the graph here, as the Interface anchor's run. False when it did not (the caller replays the
	 * display into the D3D11 renderer), with a_reason saying why: the surface is new or cannot be imported, a pipeline cannot be
	 * made, a texture cannot be resolved, or the epoch did not run with the client in it. Render thread, at EndDisplay.
	 */
	bool Submit(Display& a_display, const char*& a_reason);
}

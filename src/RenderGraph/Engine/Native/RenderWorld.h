#pragma once

namespace RenderGraph::Engine::Native
{
	/**
	 * @brief Native: Main::RenderWorld (SE 100424, AE 107142) driven by CS in the engine function's order, with no engine function called.
	 * The accumulator's pre-resolve and post-resolve (SE 99938 / 99939, AE 106583 / 106584) are CS's points in their order: Deferred's
	 * start and opaque end, DCLF's sky, water and blended draws, the reflections and the water blend, View Atmosphere's composite, with
	 * the resolve functions' clears and kMAIN copy made through the skeleton, and the shadow state they leave for the engine code after
	 * RenderWorld. Their points read CS's frame (WorldPass.h). RenderSSAO, the snow pass or the kMAIN copy and the composite are CS's
	 * ImageSpace steps. No light flare setup (the snapshot's screen blood block has it), no screen splatter cull or submit, no full-screen
	 * blur camera test (the native post chain keeps it), no accumulator end calls (its lists are empty in native). The sun's occlusion
	 * draw is CS's (its answer is taken game side). Render owner.
	 */
	void RenderWorld(bool a_firstPerson);
}

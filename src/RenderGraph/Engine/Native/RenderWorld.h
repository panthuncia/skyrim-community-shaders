#pragma once

namespace RenderGraph::Engine::Native
{
	/**
	 * @brief Native: Main::RenderWorld (SE 100424, AE 107142) driven by CS in the engine function's order. The engine's function is
	 * not entered. Its own CS points are CS calls: RenderSSAO, the copy or snow pass and the composite (ImageSpace), no light flare
	 * setup (the snapshot's screen blood block has it), no screen splatter cull or submit (the blood is drawn from its root), and no
	 * full-screen blur camera test (the native post chain keeps it), no accumulator end calls (its lists are empty in native). The sun's
	 * occlusion draw is CS's (its answer is taken game side). The accumulator's pre-resolve (inside StartAccumulating) and post-resolve
	 * are still the engine's functions, called by CS; their points read CS's frame (WorldPass.h).
	 * Render owner.
	 */
	void RenderWorld(bool a_firstPerson);
}

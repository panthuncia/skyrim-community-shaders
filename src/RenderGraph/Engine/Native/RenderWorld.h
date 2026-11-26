#pragma once

namespace RenderGraph::Engine::Native
{
	/**
	 * @brief Native: Main::RenderWorld (SE 100424, AE 107142) driven by CS in the engine function's order. The engine's function is
	 * not entered. Its own CS points are CS calls: RenderSSAO, the copy or snow pass and the composite (ImageSpace), no light flare
	 * setup (the snapshot's screen blood block has it), no screen splatter cull or submit (the blood is drawn from its root), and no
	 * full-screen blur camera test (the native post chain keeps it). The accumulator's pre-resolve (inside StartAccumulating) and
	 * post-resolve, the sun's occlusion queries and the first-person end are still the engine's functions, called by CS.
	 * Render owner.
	 */
	void RenderWorld(bool a_firstPerson);
}

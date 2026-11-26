#pragma once

namespace RenderGraph::Engine::Native
{
	/**
	 * @brief Native: Main::RenderDepth (SE 100421, AE 107139) driven by CS in the engine function's order, with no engine function called.
	 * The main depth's clear and its copy into kPOST_ZPREPASS_COPY go through the skeleton; DCLF's Z-prepass (with the views' depth work,
	 * first person's included) runs at the point after the world's depth draws, and first person's draw point takes the hidden bits and
	 * depth range. Not made: the depth accumulator's draws (nothing registers into it in native), the focus targets' stencil marks (no
	 * reader: the masks never take the focus branch), the first-person cull, its camera and node shift and its draw (DCLF draws first person
	 * from its own camera and rows). Render owner, inside the depth pass hook.
	 */
	void RenderDepth(bool a_firstPerson, bool a_controlDriven);

	/**
	 * @brief Native: Main::RenderShadowMasks (SE 100422, AE 107140) driven by CS, with no engine function called: when the scene's shadows
	 * are on, the world's masks are cleared and drawn from CS's frame (DCLF's shadow mask family). Render owner.
	 */
	void RenderShadowMasks();

	/**
	 * @brief Native: Main::RenderFirstPersonView (SE 100411, AE 107129) driven by CS, with no engine function called: DCLF's first-person
	 * colour epoch at the point of the engine's first-person accumulator render. Not made: that render and its blended groups (nothing
	 * registers into the first-person accumulator in native), the depth range and target writes around it, the sun glare's cull and
	 * draw over first person (no CS family draws the glare; the engine's draw was dropped) and the sun's second occlusion query (the
	 * first, Main::RenderWorld's, already tests against the first-person depth DCLF's Z-prepass wrote). Render owner.
	 */
	void RenderFirstPersonView();
}

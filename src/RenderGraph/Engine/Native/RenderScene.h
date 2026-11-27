#pragma once

/**
* @brief Native's world frame skeleton driven by CS (render-thread roadmap, phase 3, decision 70; notes in
* docs/development/plans/snap3-skeleton.md).
*
* Main::DrawFrame's world call of Main::RenderScene (SE 35556 +0x2DC, AE 36555 +0x4C8) goes to CS's driver in native. Neither
* Main::RenderScene (SE 35559, AE 36558) nor Main::RenderPlayerView (SE 35560, AE 36559) is entered. The driver runs, in their order:
*
* - the game-side work they do (decision 8): the reflections' camera the game reads, the main list process's camera and frustum and
*   the queued lights (ShadowSceneNode::ProcessAllQueuedLights, which reads that process), the first-person root's hidden bit around
*   them, the first-person root shift and its camera (AE 107123 / 107124, which DCLF's first-person camera reads), the sky colours the
*   lighting shader and the SAO and snow helpers keep, and Main::Draw's tail (AE 107143: the sun's query flag and the engine's per-frame
*   releases);
* - CS's points, through the call sites' hook chains (EngineZones::CallTarget, decision 57): DCLF's frame start, the shadow casters,
*   the shadow maps, the depth pass, the masks, the precipitation, RenderWorld, the first-person view, the post chain and the screen
*   splatter. In native none of them enters the engine's function;
* - the skeleton's clears before RenderWorld (the normals target, kMAIN, and the snow target with improved snow), through CS's skeleton.
*
* Not made: the render state bookkeeping that only fed the engine's own render functions (SetCameraData and the camera cache, the clear
* colours, the target binds, the dirty bits, the first-person accumulator's and BSShaderManager::State's eye, the list processes' resets,
* the scene list and clear jobs, the map menu flag, the refraction group's count and clears). Guest installs nothing.
*/
namespace RenderGraph::Engine::Native::Scene
{
	/** @brief Native only: Main::DrawFrame's world call of Main::RenderScene goes to CS's driver. */
	void InstallHooks();

	/** @brief Whether native's world frames run CS's driver in place of Main::RenderScene. */
	bool Owns();
}

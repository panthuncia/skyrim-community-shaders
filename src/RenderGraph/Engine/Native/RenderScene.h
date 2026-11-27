#pragma once

#include <array>
#include <cstdint>

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
	/**
	* @brief What the driver's game half leaves for its render half (render-thread roadmap, phase 7): the branches, the values the
	* points read of the game, and the first-person camera as the shift placed it. Values only, apart from the SAO helper, which only
	* goes back to the engine's RenderSaoCs. The game half runs first, on the game thread; the render half reads this, not the game.
	*/
	struct Prepared
	{
		bool valid = false;          // a world frame's game half ran
		bool firstPerson = false;    // the first-person view is drawn
		bool controlDriven = false;  // the movement controller's vfunc +0x78: RenderDepth's and RenderFirstPersonView's second argument
		bool menuMode = false;       // UI +0x160 above 0: RenderWorld's and RenderFirstPersonView's first argument
		// The clears before RenderWorld: the normals target's (c, c, 0, 0), kMAIN's colour, and whether improved snow clears its target.
		float normalsClear = 0.0f;
		std::int32_t normalsTarget = 4;  // the normals target this frame (WorldPass::NormalsTarget's rule)
		std::array<float, 3> mainClear{};
		bool snowClear = false;
		// The end-of-frame ImageSpace runs (414573 and gEnableEOFImageSpace); the compute SAO's helper and flag; the screen splatter.
		bool endOfFrame = false;
		void* saoHelper = nullptr;
		bool saoCompute = false;
		bool splatter = false;
		// The first-person camera's near and far after the shift back, which the depth of field takes (415007 +0x160, +0x164).
		bool dofCamera = false;
		float dofNear = 0.0f;
		float dofFar = 0.0f;
		/** @brief The first-person camera (415007) between the shift and the shift back, and Main::DrawWorld's eye (415027). */
		struct FirstPersonCamera
		{
			bool valid = false;
			RE::NiTransform world{};
			RE::NiFrustum frustum{};
			std::array<float, 4> port{};  // NiRect<float>: left, right, top, bottom
			std::array<float, 3> eye{};
		} firstPersonCamera;
	};

	/** @brief Native only: Main::DrawFrame's world call of Main::RenderScene goes to CS's driver. */
	void InstallHooks();

	/** @brief Whether native's world frames run CS's driver in place of Main::RenderScene. */
	bool Owns();

	/** @brief The current world frame's game half, for its render half. Render side. Invalid outside a native world frame's render half. */
	const Prepared& Current();
}

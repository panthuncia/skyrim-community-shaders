#pragma once

#include "Snapshot/FrameSnapshot.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <vector>

namespace RE
{
	class Main;
	class NiCamera;
	class TESWaterSystem;
}

/**
* @brief Native's Main::RenderPlayerView without SetupSunAndSky's and RenderWaterEffects' engine calls (render-thread roadmap, phase 3;
* notes in docs/development/plans/snap3-water.md). Decision 8: the game half runs on the game thread, the render half is CS's.
*
* - Main::SetupSunAndSky (SE 35562, AE 36561) is CPU work on the scene graph and the camera (the sky nodes follow the camera, the FOV,
*   the culler setup, DrawWorld's player data). The capture runs it on world snapshots, before the main camera is captured. Main::Draw's
*   call of it is CS's point (DCLF's frame start), which calls no engine function in native.
* - Main::RenderWaterEffects (SE 35561, AE 36560) is not called by Main::Draw in native. The capture runs its game half: the early-out
*   resets (S20), TESWaterSystem::Update (game logic) and the flow-map ring (S16). Inside the update, TESWaterReflections::Update runs
*   with its face render recorded instead of drawn, TESWaterSystem::RenderDisplacement is ported (its CPU part and the displacement
*   effect's), and the water visibility test is a frustum test against the snapshot camera (no engine cull).
* - What the game half asks to draw (the reflection faces, the displacement steps, the flow copies) is queued with the snapshot id, like
*   the UI uploads (decision 62): the render side applies every queued item up to the snapshot it adopted, once, at CS's point. A
*   repeated snapshot draws none of it again; a replaced one's work is drawn with the next.
*
* Guest installs nothing.
*/
namespace RenderGraph::Engine::Native::WaterEffects
{
	/** @brief One cube face TESWaterReflections::Update asked for: the cube camera as the update placed it. */
	struct Face
	{
		std::uint32_t face = 0;
		RE::NiPoint3 translate{};
		float scale = 1.0f;
		RE::NiFrustum frustum{};
		std::array<float, 4> port{};  // NiRect<float>: left, right, top, bottom
		bool plain = false;           // mode 0: no silhouettes and the sky drawn
	};

	/** @brief One step of the displacement simulation, with the rows its effect's param holds when the engine would draw it. */
	struct DisplacementStep
	{
		enum class Kind : std::uint8_t
		{
			TexOffset,  // ISWaterDisplacementTexOffset
			Ripple,     // ISWaterDisplacementWadingRipple: two vertex rows, the ripple's placement
			Heightmap,  // ISWaterWadingHeightmap
		};
		Kind kind = Kind::TexOffset;
		std::uint32_t count = 0;                     // ids set (pixel ids for the quads, vertex rows for a ripple)
		std::array<std::array<float, 4>, 16> ids{};  // the param's float4 per id, in id order
	};

	/** @brief TESWaterSystem::RenderDisplacement's draws for one update. */
	struct Displacement
	{
		bool clear = false;                    // kWATER_DISPLACEMENT cleared to 0.5 first (the simulation restarted): ClearSimulation's draw
		std::int32_t target = -1;              // the first step's output: 0x1433dcf10 when the effect starts
		const void* rippleGeometry = nullptr;  // the effect's quad (+0x90); the effect holds it for the process's life
		std::vector<DisplacementStep> steps;
	};

	/** @brief One flow-map tile copy (ISCopySubRegionCS) of a loaded flow texture into the flow map. */
	struct FlowCopy
	{
		winrt::com_ptr<ID3D11ShaderResourceView> source;
		std::array<std::uint32_t, 8> region{};  // the copy's CS b0 block: source x, y, width, height, destination x, y, 0, 0
	};

	/** @brief What one game update's water game half asks the render side to draw. */
	struct Work
	{
		std::uint64_t snapshot = 0;
		std::vector<Face> faces;
		Displacement displacement;
		std::vector<FlowCopy> copies;
		bool clearCube = false;  // the water reflection cube is cleared first (Water Reflection Cubemap Fix: the scene changed)
	};

	/** @brief Native only, at load, before EngineZones installs (its stubs then call what this writes). */
	void InstallHooks();

	/** @brief Whether native owns the water update and SetupSunAndSky (InstallHooks ran). Any thread. */
	bool Owns();

	/**
	* @brief Game thread, inside the capture, on a world snapshot before the main camera is captured: Main::SetupSunAndSky. Its result,
	* which Main::Draw keeps, is SunAndSkyResult().
	*/
	void SetupSunAndSky();

	/** @brief What the last SetupSunAndSky returned. Main::Draw's call of it returns this in native. */
	std::uint64_t SunAndSkyResult();

	/** @brief Game thread, inside the capture, after the core block: RenderWaterEffects' game half for a world snapshot. */
	void Capture(const Snapshot::FrameSnapshot& a_snapshot);

	/**
	* @brief Render thread, at CS's frame start point in Main::Draw: draws the queued water work up to the adopted snapshot, in order.
	* Render owner.
	*/
	void Render();

	/** @brief One line for the snapshot log: updates, faces, steps and copies since the last line. */
	std::string TakeStatsLine();
}

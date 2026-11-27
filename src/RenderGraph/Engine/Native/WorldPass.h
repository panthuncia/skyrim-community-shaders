#pragma once

#include "Snapshot/FrameSnapshot.h"

#include <array>
#include <optional>
#include <cstdint>

/**
* @brief The render state CS's points inside native's Main::RenderWorld draw with, from CS's own frame instead of the shadow state the
* engine's accumulator functions set around them (decision 107). Each point opens a Scope; its readers ask Current(). Outside native's
* RenderWorld, or in guest, Current() is null and the readers keep the shadow state. Render thread.
*
* The values are what the engine's pre-resolve and post-resolve (SE 99938/99939, AE 106583/106584) set before each point for the main
* accumulator: they do not depend on the scene.
*/
namespace RenderGraph::Engine::Native::WorldPass
{
	enum class Point : std::uint8_t
	{
		SkyBatch,      // the sky techniques' batch inside the deferred pass
		WaterMask,     // the water mask's batch
		WaterSurface,  // geometry group 10 and the water techniques' batch
		SunQuery,      // the sun's occlusion draw after the post-resolve
		Count
	};

	struct State
	{
		std::array<std::int32_t, 3> colours{ -1, -1, -1 };  // RE::RENDER_TARGET, -1 for none
		std::int32_t depth = 0;                             // RE::RENDER_TARGETS_DEPTHSTENCIL
		std::uint32_t depthMode = 0, stencil = 0, stencilRef = 0, fill = 0, cull = 0, depthBias = 0, scissor = 0;
		std::uint32_t blend = 0, alphaToCoverage = 0, write = 0, blendExtra = 0;
		bool alphaTest = false;
		float alphaRef = 128.0f / 255.0f;  // the pre-resolve sets 128/255 before the sky techniques; nothing in RenderWorld changes it after
		bool shifted = false;              // inside the resolve's LOD depth shift window (SE 100407 to 100408, AE 107125 to 107126)
	};

	/**
	* @brief What the points read of game data, captured on the game thread (native world snapshots): the LOD depth shift. Inside its
	* windows the engine's resolve functions lower the depth range's maximum to a constant (SE 100407 / AE 107125) while the console's
	* ToggleDepthShift is on, and narrow the world camera's near plane for the engine's own LOD draws, which native does not make.
	*/
	struct Block
	{
		static constexpr Snapshot::BlockId kId = Snapshot::BlockId::RenderWorld;
		bool depthShift = false;  // the shift applies this update
		float shiftedMax = 1.0f;  // the depth range's maximum inside a window
		// The scene graph's near and far distances and the shift's scale (SE 304860 / AE 254903), from which the window's far plane is
		// computed.
		float nearDistance = 0.0f, farDistance = 0.0f, shiftScale = 0.0f;
		bool operator==(const Block&) const = default;
	};

	/** @brief Registers the block's capture. Native only, at load. */
	void InstallCapture();

	/**
	* @brief The depth range of the main camera's draws, in (a_shifted) or out of a depth shift window, from the adopted snapshot's block:
	* SetCameraData's (0, 1), with the window's maximum. Render thread.
	*/
	std::array<float, 2> DepthRange(bool a_shifted);

	/**
	* @brief The world camera's near and far planes, in (a_shifted) or out of a depth shift window: inside a window the engine rebuilds
	* the camera's frustum with a nearer far plane (SE 100407 / AE 107125) and restores it after; outside, the snapshot camera's.
	* Render thread.
	*/
	std::array<float, 2> CameraPlanes(bool a_shifted);

	/** @brief Marks a depth shift window of native's RenderWorld driver (the resolve functions' SE 100407 to 100408, AE 107125 to 107126). */
	class ShiftWindow
	{
	public:
		ShiftWindow();
		~ShiftWindow();
		ShiftWindow(const ShiftWindow&) = delete;
		ShiftWindow& operator=(const ShiftWindow&) = delete;
	};

	/** @brief Whether native's RenderWorld driver is inside a depth shift window on this thread. */
	bool InShiftWindow();

	/**
	* @brief The world camera's planes as the engine's camera holds them now: CameraPlanes inside native's RenderWorld (shifted inside a
	* window), else null, and the caller reads the engine's current camera. Render thread.
	*/
	std::optional<std::array<float, 2>> CurrentCameraPlanes();

	/** @brief The point a native RenderWorld point runs at, while in scope. Nests; a scope outside native's RenderWorld sets nothing. */
	class Scope
	{
	public:
		explicit Scope(Point a_point);
		~Scope();
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		Point previous;
		bool set = false;
	};

	/** @brief The state of the point in scope, or null outside every scope. */
	const State* Current();

	/**
	* @brief The normals target the frame writes (GetImageSpaceRenderTarget_TAAMASK_SSRMASK_SWAP, SE 100460 / AE 107177): native's render
	* half reads its game half's value; elsewhere the engine's rule.
	*/
	std::int32_t NormalsTarget();

	/** @brief The engine's rule for the normals target (100460): the swap on even frames while SSR is on, else the main one. */
	constexpr std::int32_t NormalsTargetOf(bool a_ssr, std::uint32_t a_frameCounter)
	{
		return a_ssr ? static_cast<std::int32_t>((~a_frameCounter & 1u) | 4u) : 4;
	}

	/** @brief The normals target from the engine's SSR switch and frame counter, read now. Guest. */
	std::int32_t EngineNormalsTarget();

	/** @brief Whether native's RenderWorld is running on this thread. */
	bool InRenderWorld();

	/**
	* @brief The targets the main accumulator renders to when the pre-resolve starts (Deferred::StartDeferred keeps them as its forward
	* targets): kMAIN, kMOTION_VECTOR, the normals target, none.
	*/
	std::array<RE::RENDER_TARGET, 4> ForwardTargets();

	/**
	* @brief The post-resolve's test for the water blend (SE 99939, AE 106584): the surface draws into kRAW_WATER and the blend runs.
	* a_flags are the post-resolve's render flags. Render thread, after the water mask point.
	*/
	bool WaterBlendRuns(std::uint32_t a_flags);

	/** @brief TESWaterSystem's state from the adopted snapshot (ImageSpace's inputs): 1 or 2 under water, else 0. Render owner. */
	std::int32_t Underwater();

	/** @brief The driver's water blend test for this render (WaterBlendRuns), kept until the next: the water surface's raw-water flag. */
	void SetRawWater(bool a_raw);

	/** @brief The last SetRawWater (the engine's global 528167 / 415112 kept it the same way). Render thread. */
	bool RawWater();

	/**
	* @brief TAA's form, which the engine kept at ImageSpaceManager +0x1f0 +0x2c: 1 once the water mask point ran, 0 again after TAA's
	* step. The water blend's test and the post chain read it. Native, render thread.
	*/
	std::int32_t TaaForm();

	/** @brief Sets TaaForm: the water mask point (1) and TAA's step (0). */
	void SetTaaForm(std::int32_t a_form);

	/** @brief Whether native's RenderDepth driver is running on this thread. */
	bool InRenderDepth();

	/** @brief Marks native's RenderDepth driver: DCLF's depth capture takes the main depth from CS's frame inside it. */
	class RenderDepthScope
	{
	public:
		RenderDepthScope();
		~RenderDepthScope();
		RenderDepthScope(const RenderDepthScope&) = delete;
		RenderDepthScope& operator=(const RenderDepthScope&) = delete;
	};

	/** @brief Marks native's RenderWorld: scopes set their point only inside it. */
	class RenderWorldScope
	{
	public:
		RenderWorldScope();
		~RenderWorldScope();
		RenderWorldScope(const RenderWorldScope&) = delete;
		RenderWorldScope& operator=(const RenderWorldScope&) = delete;
	};
}

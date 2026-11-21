#pragma once

// Water drawn by DCLF (docs/development/plans/dclf-water-indirect.md, phases 2 and 3): the Water program family and its two deferred
// ranges, the mask (the Stencil technique) and the surface (techniques 0, 9 and 11). Water.hlsl's DCLF_BINDLESS path draws both.
// - The family's keys come from the water port (Features/Water/Scene). Each key gets one plain pipeline per range, with the engine's
//   blend, depth and stencil state (DrawPipelines' TryRequestDeferred and BuildDeferred).
// - A key DCLF cannot draw in both ranges fails its build, so its objects stay the latch path's.
// - The ranges are culled in the colour segment and drawn by the water.mask and water.surface epochs (WaterEpochs.cpp), triggered
//   at the engine's two water batches. Each range picks its targets by frame: the normals mask or its swap, kMAIN or kRAW_WATER.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <cstdint>

namespace DCLF::Features::Water
{
	enum class View : std::uint8_t;
}

namespace DCLF::Features::Water::Indirect
{
	/** @brief The water object rows' register (Water.hlsl's kDCLFWaterObjectsRegister), served by WaterSceneRows.cpp's frame binding. */
	inline constexpr std::uint32_t kObjectRowsRegister = 42;

	/** @brief The frame textures Water.hlsl's DCLF path reads in place of the engine's per-draw slots. */
	inline constexpr std::uint32_t kRefractionRegister = 46;    // t1: kMAIN's copy
	inline constexpr std::uint32_t kDisplacementRegister = 47;  // t2: kWATER_DISPLACEMENT
	inline constexpr std::uint32_t kDepthRegister = 48;         // t7: kPOST_ZPREPASS_COPY
	inline constexpr std::uint32_t kFlowMapRegister = 49;       // t8: kWATER_FLOW, or the ImageSpace port's flow copy
	inline constexpr std::uint32_t kSsrRegister = 56;           // t10: kSSR
	inline constexpr std::uint32_t kSsrRawRegister = 75;        // t11: kSSR_RAW
	inline constexpr std::array<std::uint32_t, 6> kFrameTextureRegisters{ kRefractionRegister, kDisplacementRegister, kDepthRegister, kFlowMapRegister, kSsrRegister,
		kSsrRawRegister };

	/**
	 * @brief The mask's stencil mode for water-type bit 1 << index is kMaskStencilModeBase + index (SetupGeometry's modes 2-9 write the
	 * bit). Unverified: the pipelines check the engine's state's write mask against the bit, so a wrong base keeps water native.
	 */
	inline constexpr std::uint8_t kMaskStencilModeBase = 2;
	/** @brief The surface's depth and stencil modes (p5/water.md): depth tested without writes, NOT_EQUAL on bit 0 with reference 1. */
	inline constexpr std::uint8_t kSurfaceDepthMode = 1, kSurfaceStencilMode = 30;
	/** @brief The mask's depth mode, and the write modes: the mask's RT0 blue and RT1, the surface's kMAIN. */
	inline constexpr std::uint8_t kMaskDepthMode = 3, kMaskWriteMode = 9, kSurfaceWriteMode = 1;

	/**
	 * @brief Install time, main thread. Registers the program family, the two deferred ranges, their frame textures and the drawn
	 * input. Only while the water port is registered (WaterScene::On), since a program family needs its material port.
	 */
	void Register(Api::InstallContext& a_context);

	/**
	 * @brief The classify input "water drawn by dclf": on while a draw client is set (SetDrawer) and the frame is GPU-driven. The
	 * CPU-driven pipeline has no kViewWater input, so water claimed there would be drawn by nobody. The port's derive must refuse to
	 * claim water while it is off.
	 */
	Api::ClassifyInputId DrawnInput();

	/** @brief Any thread. Whether something draws the deferred ranges. Set once the water epochs' triggers are hooked. */
	void SetDrawer(bool a_drawer);

	/** @brief Install time. Finds the water epochs' triggers and registers their report lines. Only with Register. */
	void RegisterEpochs(Api::InstallContext& a_context);

	/**
	 * @brief Render thread, at the engine's mask or water batch, before the engine runs it. Submits the view's epoch inside its trigger
	 * for the main camera's batch. Returns whether it was submitted. The latch path draws the rest of the batch after it.
	 */
	bool ReachBatch(View a_view, const void* a_accumulator);
}

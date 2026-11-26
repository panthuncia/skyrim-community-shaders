#pragma once

// Water drawn by DCLF: the Water program family and its two deferred
// ranges, the mask (the Stencil technique) and the surface (techniques 0, 9 and 11). Water.hlsl's DCLF_BINDLESS path draws both.
// - The family's keys come from the water port (Features/Water/Scene). Each key gets one plain pipeline per range, with the engine's
//   blend, depth and stencil state (DrawPipelines' TryRequestDeferred and BuildDeferred).
// - A key DCLF cannot draw in both ranges fails its build, so its objects stay the batch hooks'.
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
	inline constexpr std::uint32_t kFlowMapRegister = 49;       // t8: the flow map source (Unified Water's world flow map, or the ImageSpace port's map)
	inline constexpr std::uint32_t kSsrRegister = 56;           // t10: kSSR
	inline constexpr std::uint32_t kSsrRawRegister = 75;        // t11: kSSR_RAW
	inline constexpr std::uint32_t kGlobalCubeRegister = 54;    // t3 for a material that reads the global cube: kREFLECTIONS
	inline constexpr std::array<std::uint32_t, 7> kFrameTextureRegisters{ kRefractionRegister, kDisplacementRegister, kDepthRegister, kFlowMapRegister, kSsrRegister,
		kSsrRawRegister, kGlobalCubeRegister };

	/**
	 * @brief The mask's stencil mode for water-type bit 1 << index is kMaskStencilModeBase + index (SetupGeometry's modes 2-9 write the
	 * bit). Unverified: the pipelines check the engine's state's write mask against the bit, so a wrong base keeps water native.
	 */
	inline constexpr std::uint8_t kMaskStencilModeBase = 2;
	/**
	 * @brief The depth modes. The engine's mask tests and writes depth (3) and its surface tests it (1), which needs both passes to
	 * reach the same depth. DCLF's mask and surface pipelines land one or two depth steps apart on the same water (water plan,
	 * section 11), so the surface failed in bands. Here the mask only tests (1) and the surface tests and writes (3). The depth
	 * after the surface holds the nearest water either way, and nothing between the two epochs reads kMAIN's depth.
	 * The surface's stencil mode (engine/water.md) is NOT_EQUAL on bit 0 with reference 1.
	 */
	inline constexpr std::uint8_t kSurfaceDepthMode = 3, kSurfaceStencilMode = 30;
	/** @brief The mask's depth mode (see kSurfaceDepthMode), and the write modes: the mask's RT0 blue and RT1, the surface's kMAIN. */
	inline constexpr std::uint8_t kMaskDepthMode = 1, kMaskWriteMode = 9, kSurfaceWriteMode = 1;

	/**
	 * @brief Install time, main thread. Registers the program family, the two deferred ranges, their frame textures and the drawn
	 * input. Only while the water port is registered (WaterScene::On), since a program family needs its material port.
	 */
	void Register(Api::InstallContext& a_context);

	/**
	 * @brief The classify input "water drawn by dclf": on while a draw client is set (SetDrawer), in GPU-driven frames (the deferred range
	 * passes) and CPU-driven ones (the CPU pipeline's water sites). The port's derive must refuse to claim water
	 * while it is off.
	 */
	Api::ClassifyInputId DrawnInput();

	/** @brief Any thread. Whether something draws the deferred ranges. Set once the water epochs' triggers are hooked. */
	void SetDrawer(bool a_drawer);

	/** @brief Install time. Finds the water epochs' triggers and registers their report lines. Only with Register. */
	void RegisterEpochs(Api::InstallContext& a_context);

	/**
	 * @brief Render thread, at the engine's mask or water batch, before the engine runs it. Submits the view's epoch inside its trigger
	 * for the main camera's batch. Returns whether it was submitted. The latch path draws the rest of the batch after it. In a
	 * CPU-driven frame it submits nothing and hands the colour commit's water draws to a_cpuRun (set set a_cpuVariant), for the water
	 * family's run to carry into its anchor pass.
	 */
	bool ReachBatch(View a_view, const void* a_accumulator, void** a_cpuRun = nullptr, std::uint32_t* a_cpuVariant = nullptr);

	/**
	 * @brief Render thread, at FinishAccumulating's check for water mask passes. Whether the mask batch must run for DCLF even though the
	 * engine holds no mask pass: the main camera's batch renderer while the mask epoch is live. The engine skips the whole mask block
	 * (the stencil clear, the targets, the batch that triggers the epoch) when every water shape is DCLF's.
	 */
	bool MaskBatchWanted(const void* a_batchRenderer);
}

#pragma once

// TruePBR: while loaded, its Lighting shader hooks replace SetupMaterial for PBR passes and change the passes themselves.
// The integration registers a Replace material contributor that hands passes back to vanilla on CHECK_PBR_TEXTURE,
// its own material classes, its landscape layers at t80-t91, glints property bits, Lighting pass rules, the glints noise at t20 and a parity.
// The pass rules apply whether or not TruePBR is loaded. The contributor is only active while it is loaded.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <cstdint>

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF::Integrations::TruePBR
{
	// t80-t91: its landscape displacement (t80-t85) and RMAOS (t86-t91) layers, the contributor's registers in static storage.
	inline constexpr std::array<std::uint32_t, 12> kTextureRegisters{ 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91 };
	// t20: the glints noise.
	inline constexpr std::uint32_t kGlintsNoiseRegister = 20;
	// Its contributor bit in the Lighting property record: the material's glint is enabled.
	inline constexpr std::uint8_t kGlints = 1u << 0;

	/** @brief Whether a vtable belongs to this DLL, meaning one of CS's own material classes. */
	bool OwnClass(const void* a_vtable);
	/** @brief Bytes Capture takes of CS's PBR classes, the landscape size for feature 19. */
	std::uint32_t PBRMaterialBytes(const RE::BSShaderMaterial& a_material);
	/** @brief The PBR classes' texture fields: landscape has 4 per tile plus overlay and noise, PBR the base four plus five maps. */
	void PBRTextureFields(const RE::BSShaderMaterial& a_material, std::uint16_t* a_out, std::uint32_t& a_count);
	/** @brief Render thread, at the frame's start. Active while TruePBR is loaded, since its hooks only install then. */
	void SampleFrame(Api::MaterialContributorFrame& a_frame);
	/** @brief Whether TruePBR's hook takes the pass: TruePbr set, and the technique is not LODLand or LODLandNoise. */
	bool PBRTakesPass(const Api::MaterialContributorEvaluation& a_evaluation);
	/** @brief TruePBR::BSLightingShader_SetupMaterial for a pass it takes. Returns Made, Uncovered, or HandBack on CHECK_PBR_TEXTURE. */
	Api::MaterialOutcome EvaluatePBR(Api::MaterialContributorEvaluation& a_evaluation);

	Api::Feature Integration();
}

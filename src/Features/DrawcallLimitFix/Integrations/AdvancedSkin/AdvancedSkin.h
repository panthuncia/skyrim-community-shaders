#pragma once

// Advanced Skin's SkinPerGeometry (PS b7, the owning actor's wetness) as a shading field of the published actor-owned class.
// Each actor's value is captured once per frame at the frame's start and copied to its meshes. Every other slot is zero.
// Its t71 and t74 material textures are an After material contributor, declared with SkinPerGeometry's per-draw buffer.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace DCLF::Integrations::AdvancedSkin
{
	// Lighting.hlsl's t71 (RFAOS) and t74 (wetness), the contributor's registers in ascending order. Core assigns their slots.
	inline constexpr std::uint32_t kRfaosRegister = 71;
	inline constexpr std::uint32_t kWetnessRegister = 74;
	inline constexpr std::array<std::uint32_t, 2> kMaterialTextureRegisters{ kRfaosRegister, kWetnessRegister };
	// SkinPerGeometry (PS b7), bound per draw by its SetupGeometry hook for the actor the engine drew last.
	inline constexpr std::uint32_t kSkinPerGeometryRegister = 7;

	void InstallMaterialTextures(Api::InstallContext& a_context);

	Api::Feature Integration();

	/** @brief Render thread. The wetness index's sizes and the last capture's counts, as "<name> <value>" lines. */
	std::vector<std::string> GrowthLines();
}

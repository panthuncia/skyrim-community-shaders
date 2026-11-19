#pragma once

// Advanced Skin's SkinPerGeometry (PS b7, the owning actor's wetness) as a shading field of the published actor-owned class.
// Each actor's value is captured once per frame at the frame's start and copied to its meshes. Every other slot is zero.
// Its per-geometry material (b7's flags and profile, t71, t74, t72, s15) is a table row each skin surface's shading row names.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct ID3D11ShaderResourceView;

namespace DCLF::Integrations::AdvancedSkin
{
	// SkinPerGeometry (PS b7), bound per draw by its SetupGeometry hook for the actor the engine drew last.
	inline constexpr std::uint32_t kSkinPerGeometryRegister = 7;
	// The material table, a frame binding. Skin.hlsli's DCLFSkinMaterials declares the same register.
	inline constexpr std::uint32_t kMaterialTableRegister = 38;
	// Skin's per-geometry textures, in the order of a row's descriptors.
	inline constexpr std::array<std::uint32_t, 3> kMaterialTextureRegisters{ 71, 74, 72 };

	/** @brief One material table row, Skin.hlsli's DCLFSkinMaterialRow. */
	struct MaterialRow
	{
		float materialFlags[4];        // b7's: t71 bound, t74 bound, t72 is a BC5 normal-only map, thickness
		float profile[28];             // b7's Skin::SkinData
		std::uint32_t descriptors[4];  // t71, t74, t72 views and the s15 sampler, each heap index + 1, 0 for none
		float wetnessOverride[4];      // x: the editor's preview sweat, w: 1 when it replaces the actor's wetness
	};
	static_assert(sizeof(MaterialRow) == 160);

	/** @brief What a slot's row was built from at the last capture. Render thread. */
	struct SlotMaterial
	{
		std::array<ID3D11ShaderResourceView*, 3> views{};  // identities only, in kMaterialTextureRegisters' order
		const MaterialRow* row = nullptr;
		bool complete = false;  // every view has its descriptor, so the row matches b7
	};

	void InstallMaterials(Api::InstallContext& a_context);

	/** @brief Render thread. a_slot's material as the last capture made it. False when the slot is not an actor's. */
	bool SlotMaterialOf(std::uint32_t a_slot, SlotMaterial& a_out);

	Api::Feature Integration();

	/** @brief Render thread. The wetness index's sizes and the last capture's counts, as "<name> <value>" lines. */
	std::vector<std::string> GrowthLines();
}

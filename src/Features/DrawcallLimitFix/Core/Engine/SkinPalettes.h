#pragma once

#include <cstdint>

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"

namespace DCLF::SkinPalettes
{
	inline constexpr std::uint32_t kRowsPerBone = 3;  // float4 rows per bone
	inline constexpr std::uint32_t kMaxRows = 240;  // the engine's palette limit, 80 bones

	/** @brief The palette rows a skin's record needs, 0 without skin data. */
	inline std::uint32_t RowsOf(const SceneCapture::GeometryRecord& a_geometry)
	{
		return a_geometry.skinData ? a_geometry.boneCount * kRowsPerBone : 0u;
	}
	/** @brief Whether a skin of a_rows takes a palette block, within the engine's limit. */
	inline bool Fits(std::uint32_t a_rows) { return a_rows && a_rows <= kMaxRows; }

	/**
	 * @brief The engine's per-frame palette update (AE FUN_140e4ff90), which an owned skin would otherwise miss. Idempotent per frame
	 * under the skin's critical section, so any thread may run it.
	 */
	inline void Update(RE::NiSkinInstance* a_skin, const RE::NiTransform& a_world)
	{
		using UpdateSkinInstance = void (*)(RE::NiSkinInstance*, const RE::NiTransform*);
		static const REL::Relocation<UpdateSkinInstance> updateSkinInstance{ REL::Offset(Engine::Address::kUpdateSkinInstance) };
		updateSkinInstance(a_skin, &a_world);
	}

	// False when its size mismatches its block.
	inline bool Of(const RE::BSGeometry& a_geometry, std::uint32_t a_rows, const float*& a_current, const float*& a_previous)
	{
		auto* skin = a_geometry.GetGeometryRuntimeData().skinInstance.get();
		if (!skin)
			return false;
		a_current = static_cast<const float*>(skin->boneMatrices);
		a_previous = static_cast<const float*>(skin->prevBoneMatrices);
		return a_current && a_previous && skin->numMatrices * 3 == a_rows;
	}

	/** @brief Under the caller's lease. Runs the palette update, then reads the palettes. False when the size is not a_rows. */
	inline bool Sample(const RE::BSGeometry& a_geometry, std::uint32_t a_rows, const float*& a_current, const float*& a_previous)
	{
		if (auto* skin = a_geometry.GetGeometryRuntimeData().skinInstance.get())
			Update(skin, a_geometry.world);
		return Of(a_geometry, a_rows, a_current, a_previous);
	}
}

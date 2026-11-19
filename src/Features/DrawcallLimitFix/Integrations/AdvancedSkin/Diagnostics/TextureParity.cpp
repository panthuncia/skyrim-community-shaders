// Advanced Skin's material parity: t71, t74, t72 and b7's flags and profile. Render thread.

#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/Diagnostics/TextureParity.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"

#include <array>
#include <cstring>

#include "Features/Skin.h"
#include "Globals.h"

namespace DCLF::Integrations::AdvancedSkin
{
	bool Loaded()
	{
		return globals::features::skin.loaded;
	}

	void DrawSkinTextureParity::Compare(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex,
		[[maybe_unused]] const SceneTables& a_tables, NoteFn a_note, void* a_owner)
	{
		SlotMaterial expected;
		if (!SlotMaterialOf(a_objectIndex, expected))
			return;
		// Bound by State::Draw from SetupGeometry's resolve, which clears them first, so they are this draw's.
		std::array<ID3D11ShaderResourceView*, kMaterialTextureRegisters.size()> native{};
		for (std::size_t f = 0; f < native.size(); ++f)
			a_context->PSGetShaderResources(kMaterialTextureRegisters[f], 1, &native[f]);
		++checks;
		// The capture runs before the frame's Prepass, so a geometry differs on the frame Skin first prepares it.
		if (native != expected.views) {
			++mismatches;
			a_note(a_owner, a_geometry, fmt::format("skin textures: DCLF t71 {} t74 {} t72 {}, native t71 {} t74 {} t72 {}", fmt::ptr(expected.views[0]),
				fmt::ptr(expected.views[1]), fmt::ptr(expected.views[2]), fmt::ptr(native[0]), fmt::ptr(native[1]), fmt::ptr(native[2])));
		}
		for (auto* bound : native) {
			if (bound)
				bound->Release();
		}
		// A row still missing a descriptor has those flags off on purpose.
		if (!expected.complete || !expected.row)
			return;
		const auto& b7 = globals::features::skin.lastGeometryData;
		++checks;
		if (std::memcmp(expected.row->materialFlags, &b7.materialFlags, sizeof(expected.row->materialFlags)) != 0 ||
			std::memcmp(expected.row->profile, &b7.profile, sizeof(expected.row->profile)) != 0) {
			++mismatches;
			const auto& flags = expected.row->materialFlags;
			a_note(a_owner, a_geometry, fmt::format("skin material: DCLF flags ({} {} {} {}) detail {}, native flags ({} {} {} {}) detail {}", flags[0], flags[1],
				flags[2], flags[3], expected.row->profile[11], b7.materialFlags.x, b7.materialFlags.y, b7.materialFlags.z, b7.materialFlags.w,
				b7.profile.skinDetailParams.w));
		}
	}
}

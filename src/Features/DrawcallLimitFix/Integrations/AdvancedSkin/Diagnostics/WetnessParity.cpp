// Advanced Skin's wetness parity under CS_DCLF_CAPTURE_PARITY. Render thread.

#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/Diagnostics/WetnessParity.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include "Features/Skin.h"

#include <algorithm>
#include <string>

namespace DCLF::Integrations::AdvancedSkin
{
	namespace
	{
		const std::vector<std::uint32_t>& Members(const SceneTables& a_tables)
		{
			return a_tables.classMembers[static_cast<std::size_t>(Api::EntryClass::ActorOwned)].objects;
		}
	}

	void DrawWetnessParity::Compare(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables,
		const org::async::KeptChunks<BindlessShading>* a_rows, NoteFn a_note, void* a_owner)
	{
		// b7, the SetupGeometry hook's upload, bound only while enabled.
		auto& skin = globals::features::skin;
		if (skin.settings.EnableSkin && a_rows && a_objectIndex < a_rows->size()) {
			const auto& expected = (*a_rows)[a_objectIndex].skinPerGeometry;
			const auto& native = skin.lastGeometryData.skinPerGeometry;
			++checks;
			if (native.x != 0.0f || native.y != 0.0f)
				++wetDraws;
			if (native.x != expected[0] || native.y != expected[1] || native.z != expected[2] || native.w != expected[3]) {
				++mismatches;
				a_note(a_owner, a_geometry, fmt::format("skin wetness: DCLF ({} {} {} {}), native ({} {} {} {})", expected[0], expected[1], expected[2],
					expected[3], native.x, native.y, native.z, native.w));
			}
			const auto* owner = a_geometry->GetUserData();
			const bool actor = owner && owner->GetFormType() == RE::FormType::ActorCharacter;
			const auto& members = Members(a_tables);
			if (actor != std::binary_search(members.begin(), members.end(), a_objectIndex)) {
				++ownerMismatches;
				a_note(a_owner, a_geometry, fmt::format("skin wetness: owned by an actor {}, DCLF resolved {}", actor, !actor));
			}
		}
	}
}

// Registers the actors feature: switch, toggle, actor parent, techniques, entry class and the animation stamp.

#include "Features/DrawcallLimitFix/Features/Actors/Actors.h"

#include "ShaderCache.h"

#include <cstdint>

namespace DCLF::Features::Actors
{
	namespace
	{
		using Technique = SIE::ShaderCache::LightingShaderTechniques;

		void Install(Api::InstallContext& a_context)
		{
			NeedFadeAmounts(a_context);
			const auto actorsSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_ACTORS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: actors stay native (live toggle's seed)", .after = "CS_DCLF_SKIN_PARTITIONS" });
			a_context.RegisterSwitch({ .name = "CS_DCLF_CELL_TRACE", .kind = Api::SwitchKind::Diagnostic,
				.description = "1: each scene pass's attaches and detaches of actors' geometry, and each frame's nearby actors' parts against DCLF's claims and the engine's kept passes" });
			// Features/FirstPerson's toggle depends on this one for the player's skeleton.
			const auto actors = a_context.RegisterToggle({ .name = "actors", .label = "Actors", .seed = actorsSwitch, .entersClassification = true,
				.after = "skin partitions" });
			a_context.RegisterParent({ .parent = Api::EngineParent::Actor, .leaves = Api::ParentLeaves::AllLeaves, .toggle = actors });
			// SetupMaterial writes all of these. Face part positions come from Features/Faces.
			for (const auto technique : { Technique::FacegenRGBTint, Technique::Facegen, Technique::Hair, Technique::Eye })
				a_context.RegisterTechnique({ .technique = static_cast<std::uint32_t>(technique), .toggle = actors });
			// Checked every frame because equipment is shown, hidden and swapped with no event. They move by the skeleton,
			// with no controller or body. Published for Advanced Skin's wetness.
			a_context.RegisterEntryClass({ .entryClass = Api::EntryClass::ActorOwned, .name = "actor",
				.ownerFormType = static_cast<std::uint8_t>(RE::FormType::ActorCharacter), .everyFrame = true, .ownMotion = true, .published = true });
			RegisterActorRate(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "actors", &Install, &InstallEngineEvents };
	}
}

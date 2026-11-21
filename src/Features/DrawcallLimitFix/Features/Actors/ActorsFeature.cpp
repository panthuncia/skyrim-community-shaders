// Registers the actors feature: actor parent, techniques, entry class and the animation stamp.

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
			a_context.RegisterParent({ .parent = Api::EngineParent::Actor, .leaves = Api::ParentLeaves::AllLeaves });
			// SetupMaterial writes all of these. Face part positions come from Features/Faces.
			for (const auto technique : { Technique::FacegenRGBTint, Technique::Facegen, Technique::Hair, Technique::Eye })
				a_context.RegisterTechnique({ .technique = static_cast<std::uint32_t>(technique) });
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

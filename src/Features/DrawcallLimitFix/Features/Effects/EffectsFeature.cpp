// The Effects feature's entry. Registers its switches, object classes, the Effect port, write hooks and diagnostics.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Features::Effects
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Off by default until its shots are checked (dclf-validation.md, "Effect rigid skins").
			const auto rigidSkins = a_context.RegisterSwitch({ .name = "CS_DCLF_EFFECT_RIGID_SKINS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Never,
				.description = "1: effect meshes with a skin instance whose pass has no Skinned bit (the property without kSkinned: dog eyes) drawn by DCLF, each partition with the unskinned vertex stage and the geometry's world, as the engine draws them; 0 (default) they stay native (effect-variant (skinned))",
				.after = "CS_DCLF_PLAIN_SNOW_SPARKLE" });
			// Read once here, before any classification.
			SetPortedVariants({ .rigidSkins = SwitchEnabled(rigidSkins) });
			// Blended effects draw in the forward epoch. Opaque effects and decals draw in the G-buffer.
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Effect });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::OpaqueEffect });
			// Needed by name, because the integrations install after the features.
			NeedLightLimitFix(a_context);
			// Must register after the object class.
			RegisterPort(a_context);
			RegisterPrograms(a_context);
			RegisterFrame(a_context);
			RegisterReport(a_context);
			// Must install after Core's Lighting write hooks.
			InstallWrites();
		}
	}

	Api::Feature Feature()
	{
		return { "effects", &Install };
	}
}

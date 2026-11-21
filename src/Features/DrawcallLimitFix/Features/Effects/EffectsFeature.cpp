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
			// Registration order sets the switch list order.
			const auto opaque = a_context.RegisterSwitch({ .name = "CS_DCLF_OPAQUE_EFFECTS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on (T7f item 7): opaque BSEffectShaderProperty meshes (hint 0: no blending alpha property, no Falloff, depth tested and written; forge coals, slag) drawn by DCLF in the G-buffer with the DEFERRED effect variant, through the opaque decal group's fixed slots and its decal depth pass without the bias; 0 they stay the engine's (effect-opaque)",
				.after = "CS_DCLF_PLAIN_SNOW_SPARKLE" });
			const auto cloudLight = a_context.RegisterSwitch({ .name = "CS_DCLF_EFFECT_CLOUD_LIGHT", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on: effect meshes with kEffectLighting and kCloudLOD (lit by the cloud light, not the sun) drawn by DCLF with the cloud light's colour as a frame row; 0 they stay native (cloud-lighting)",
				.after = "CS_DCLF_OPAQUE_EFFECTS" });
			const auto falloff = a_context.RegisterSwitch({ .name = "CS_DCLF_EFFECT_FALLOFF", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on: effect meshes with Falloff and no blending alpha property (the engine's hint 1, unblended while their alpha is one) drawn by DCLF's forward epoch unblended, sorted with the blended objects; 0 they stay native (effect-opaque)",
				.after = "CS_DCLF_EFFECT_CLOUD_LIGHT" });
			// Off by default until its shots are checked (dclf-validation.md, "Effect rigid skins").
			const auto rigidSkins = a_context.RegisterSwitch({ .name = "CS_DCLF_EFFECT_RIGID_SKINS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Never,
				.description = "1: effect meshes with a skin instance whose pass has no Skinned bit (the property without kSkinned: dog eyes) drawn by DCLF, each partition with the unskinned vertex stage and the geometry's world, as the engine draws them; 0 (default) they stay native (effect-variant (skinned))",
				.after = "CS_DCLF_EFFECT_FALLOFF" });
			// Read once here, before any classification.
			SetPortedVariants({ .cloudLighting = SwitchValue(cloudLight) != "0", .falloff = SwitchValue(falloff) != "0", .rigidSkins = SwitchEnabled(rigidSkins) });
			// Blended effects draw in the forward epoch. Opaque effects and decals draw in the G-buffer.
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Effect });
			// Read once here.
			if (SwitchValue(opaque) != "0")
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

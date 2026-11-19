// The Effects feature's entry. Registers its switches, toggle, object classes, the Effect port, write hooks and diagnostics.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Effects
{
	namespace
	{
		Api::ToggleId effectsToggle;

		void Install(Api::InstallContext& a_context)
		{
			// Registration order sets the switch list order.
			const auto effects = a_context.RegisterSwitch({ .name = "CS_DCLF_EFFECTS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "1: BSEffectShaderProperty meshes drawn by DCLF: blended ones by its forward epoch, sorted with blended Lighting (T7d's Effect port; those need CS_DCLF_FORWARD), opaque ones and decals in the G-buffer; 0 they stay native (live toggle's seed)",
				.after = "CS_DCLF_FORWARD" });
			const auto opaque = a_context.RegisterSwitch({ .name = "CS_DCLF_OPAQUE_EFFECTS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on (T7f item 7): opaque BSEffectShaderProperty meshes (hint 0: no blending alpha property, no Falloff, depth tested and written; forge coals, slag) drawn by DCLF in the G-buffer with the DEFERRED effect variant, through the opaque decal group's fixed slots and its decal depth pass without the bias (needs CS_DCLF_EFFECTS and CS_DCLF_DECALS); 0 they stay the engine's (effect-opaque)",
				.after = "CS_DCLF_SHADOWS" });
			const auto cloudLight = a_context.RegisterSwitch({ .name = "CS_DCLF_EFFECT_CLOUD_LIGHT", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on: effect meshes with kEffectLighting and kCloudLOD (lit by the cloud light, not the sun) drawn by DCLF with the cloud light's colour as a frame row (needs CS_DCLF_EFFECTS); 0 they stay native (cloud-lighting)",
				.after = "CS_DCLF_EFFECTS" });
			const auto falloff = a_context.RegisterSwitch({ .name = "CS_DCLF_EFFECT_FALLOFF", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on: effect meshes with Falloff and no blending alpha property (the engine's hint 1, unblended while their alpha is one) drawn by DCLF's forward epoch unblended, sorted with the blended objects (needs CS_DCLF_EFFECTS and CS_DCLF_FORWARD); 0 they stay native (effect-opaque)",
				.after = "CS_DCLF_EFFECT_CLOUD_LIGHT" });
			// Off by default until its shots are checked (dclf-validation.md, "Effect rigid skins").
			const auto rigidSkins = a_context.RegisterSwitch({ .name = "CS_DCLF_EFFECT_RIGID_SKINS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Never,
				.description = "1: effect meshes with a skin instance whose pass has no Skinned bit (the property without kSkinned: dog eyes) drawn by DCLF, each partition with the unskinned vertex stage and the geometry's world, as the engine draws them (needs CS_DCLF_EFFECTS and CS_DCLF_SKINNED); 0 (default) they stay native (effect-variant (skinned))",
				.after = "CS_DCLF_EFFECT_FALLOFF" });
			// Read once here, before any classification.
			SetPortedVariants({ .cloudLighting = SwitchValue(cloudLight) != "0", .falloff = SwitchValue(falloff) != "0", .rigidSkins = SwitchEnabled(rigidSkins) });
			// Blended effects need the forward epoch and stay native while it is off. Opaque effects and decals draw in the G-buffer.
			// Registration order sets list order, and Particles and Billboards depend on this toggle.
			effectsToggle = a_context.RegisterToggle({ .name = "effects", .label = "Effect meshes: clouds, glows, mist",
				.tooltip = "T7d: DCLF draws blended effect-shader meshes in the forward epoch (with Blended objects), sorted with the blended Lighting objects, and (with Decals) opaque effect meshes and effect decals in its decal groups, skinned ones (with Skinned) through the skin palette. On by default (validated w174/w175; decals and skinned effects not yet).",
				.seed = effects, .entersClassification = true, .after = "forward" });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Effect, .toggle = effectsToggle });
			// Read once here, whatever the toggle.
			if (SwitchValue(opaque) != "0")
				a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::OpaqueEffect, .toggle = effectsToggle });
			// Needed by name, because the integrations install after the features.
			NeedLightLimitFix(a_context);
			// Must register after the object class.
			RegisterPort(a_context);
			RegisterPrograms(a_context);
			RegisterFrame(a_context);
			RegisterReport(a_context);
			// Must install after Core's Lighting write hooks.
			InstallWrites();
			if (Diagnostics::Observe().claims)
				SubscribeLight();
		}
	}

	Api::Feature Feature()
	{
		return { "effects", &Install };
	}

	Api::ToggleId Toggle()
	{
		return effectsToggle;
	}
}

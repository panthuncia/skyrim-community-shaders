// Forward feature registration: switches, toggles, object classes, hooks, passes and report lines.

#include "Features/DrawcallLimitFix/Features/Forward/Forward.h"

namespace DCLF::Features::Forward
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			const auto forwardSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_FORWARD", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "1: blended Lighting geometry drawn by DCLF's forward epoch (T7d: a GPU depth sort, back to front, before the engine's blended draws); 0 it stays native (live toggle's seed)",
				.before = "CS_DCLF_PROJECTED_UV" });
			const auto refractionSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_REFRACTION", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "1: refraction (kRefraction, kTempRefraction Lighting geometry: heat haze, invisibility) drawn by DCLF into the refraction normals in the forward epoch (T7d; needs CS_DCLF_FORWARD); 0 it stays native (live toggle's seed)",
				.before = "CS_DCLF_PROJECTED_UV" });
			const auto forward = a_context.RegisterToggle({ .name = "forward", .label = "Blended objects: the forward epoch",
				.tooltip = "T7d: DCLF draws blended Lighting geometry itself, sorted back to front on the GPU, just before the engine's own blended draws (effects, particles). On by default (validated w174/w175).",
				.seed = forwardSwitch, .entersClassification = true, .before = "shadows" });
			// Refraction geometry's only main pass is a Utility RENDER_NORMAL pass into kREFRACTION_NORMALS.
			const auto refraction = a_context.RegisterToggle({ .name = "refraction", .label = "Refraction: heat haze, invisibility",
				.tooltip = "T7d: DCLF draws refraction geometry's normals into the refraction normals the engine's refraction effect warps the frame by, in the forward epoch. On by default (validated w174/w175).",
				.seed = refractionSwitch, .entersClassification = true, .after = "effects", .needs = { "forward" } });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Forward, .toggle = forward });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Refraction, .toggle = refraction });
			InstallHooks(a_context);
			RegisterPasses(a_context);
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { .name = "forward", .install = &Install, .preflight = &PreflightHooks };
	}
}

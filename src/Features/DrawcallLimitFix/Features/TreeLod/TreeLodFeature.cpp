// Tree LOD: the tree LOD instances DCLF draws in the engine's place.

#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::TreeLod
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Registered where Core's switches were, to keep the order.
			const auto trees = a_context.RegisterSwitch({ .name = "CS_DCLF_LOD_TREES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: tree LOD stays native (live toggle's seed)", .before = "CS_DCLF_LOD_GATES" });
			const auto audit = a_context.RegisterSwitch({ .name = "CS_DCLF_TREE_LOD_AUDIT", .kind = Api::SwitchKind::Diagnostic, .reduced = Api::SwitchReduced::Never,
				.description = "1: tree LOD instances the engine leaves shown over a loaded, visible full tree (and hidden ones without one), every 60 frames",
				.after = "CS_DCLF_TRACE_TEXTURE_PATHS" });
			const auto tint = a_context.RegisterSwitch({ .name = "CS_DCLF_TREE_LOD_TINT", .kind = Api::SwitchKind::Diagnostic, .reduced = Api::SwitchReduced::Never,
				.description = "1: DCLF's tree LOD colour draws write solid magenta, so screen captures find its pixels (tools/dclf-corrupt-frames.py --key magenta)",
				.after = "CS_DCLF_TREE_LOD_AUDIT" });
			debugTint = SwitchEnabled(tint);
			const auto stages = a_context.RegisterSwitch({ .name = "CS_DCLF_TREE_LOD_STAGES", .kind = Api::SwitchKind::Diagnostic, .reduced = Api::SwitchReduced::Never,
				.description = "1: each stage of the main tree LOD draws (depth and colour vertex and pixel stages) stamps the frame; frames a stage missed are logged",
				.after = "CS_DCLF_TREE_LOD_TINT" });
			debugStages = SwitchEnabled(stages);
			// Registered where Core's toggle was. Its change drops no classification cache, since tree LOD is drawn from the mirror.
			const auto toggle = a_context.RegisterToggle({ .name = "tree LOD", .label = "Tree LOD: distant trees", .seed = trees, .after = "refraction" });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::TreeLod, .toggle = toggle });
			RegisterTables(a_context);
			RegisterPasses(a_context);
			RegisterFacesCull(a_context);
			RegisterFamily(a_context);
			RegisterReport(a_context);
			// CS_DCLF_PERSISTENT_PARITY: the mirror against the LOD trees root.
			if (Diagnostics::Observe().scene)
				SubscribeParity();
			// Main::Draw's early hook, DCLF running or not.
			if (SwitchEnabled(audit))
				SubscribeAudit(audit);
		}
	}

	Api::Feature Feature()
	{
		// After Core's scene events, before Core's engine side and observers.
		return { .name = "tree-lod", .install = &Install, .earlyHooks = &InstallHooks, .preflight = &PreflightHooks };
	}
}

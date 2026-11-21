// Tree LOD: the tree LOD instances DCLF draws in the engine's place.

#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Features::TreeLod
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			const auto tint = a_context.RegisterSwitch({ .name = "CS_DCLF_TREE_LOD_TINT", .kind = Api::SwitchKind::Diagnostic, .reduced = Api::SwitchReduced::Never,
				.description = "1: DCLF's tree LOD colour draws write solid magenta, so screen captures find its pixels",
				.after = "CS_DCLF_STAGE_DUMP" });
			debugTint = SwitchEnabled(tint);
			const auto stages = a_context.RegisterSwitch({ .name = "CS_DCLF_TREE_LOD_STAGES", .kind = Api::SwitchKind::Diagnostic, .reduced = Api::SwitchReduced::Never,
				.description = "1: each stage of the main tree LOD draws (depth and colour vertex and pixel stages) stamps the frame; frames a stage missed are logged",
				.after = "CS_DCLF_TREE_LOD_TINT" });
			debugStages = SwitchEnabled(stages);
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::TreeLod });
			RegisterTables(a_context);
			RegisterPasses(a_context);
			RegisterFacesCull(a_context);
			RegisterFamily(a_context);
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		// After Core's scene events, before Core's engine side and observers.
		return { .name = "tree-lod", .install = &Install, .earlyHooks = &InstallHooks, .preflight = &PreflightHooks };
	}
}

// Tree wind: the trees DCLF owns, animated on the GPU.

#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeList.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWalk.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::TreeWind
{
	namespace
	{
		Api::SwitchId listAllSwitch;  // install

		// Registered where Core's switches and toggle were, to keep the order.
		Api::ToggleId RegisterSettings(Api::InstallContext& a_context)
		{
			const auto trees = a_context.RegisterSwitch({ .name = "CS_DCLF_TREES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: trees stay native (live toggle's seed)", .after = "CS_DCLF_SKIN_PARTITIONS" });
			listAllSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_TREE_LIST_ALL", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Never,
				.description = "1 (T7f; off by default): the tree manager's list loop given DCLF's verdict for every BSTreeNode DCLF lists; off: the stood-in trees alone (T7b5's fix)",
				.after = "CS_DCLF_SHADOWS" });
			return a_context.RegisterToggle({ .name = "trees", .label = "Trees", .seed = trees, .entersClassification = true, .after = "skinned" });
		}

		void Install(Api::InstallContext& a_context)
		{
			const Api::ToggleId trees = RegisterSettings(a_context);
			RegisterObjects(a_context, trees);
			RegisterTables(a_context);
			RegisterSeeds(a_context);
			RegisterScene(a_context);
			RegisterPass(a_context);
			RegisterAnimation(a_context);
			RegisterReport(a_context);
			// CS_DCLF_PERSISTENT_PARITY: the wind rows read back against the engine's formulas and clocks.
			if (Diagnostics::Observe().scene)
				SubscribeParity();
		}

		void Preflight()
		{
			TreeList::Preflight();
			TreeWalk::Preflight();
		}
	}

	Api::Feature Feature()
	{
		return { .name = "tree-wind", .install = &Install, .preflight = &Preflight };
	}

	bool ListAll()
	{
		return SwitchEnabled(listAllSwitch);
	}
}

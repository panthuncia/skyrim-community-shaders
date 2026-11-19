// Switch nodes: the leaves under an NiSwitchNode DCLF draws while every switch on their path selects them.

#include "Features/DrawcallLimitFix/Features/SwitchNodes/SwitchNodes.h"

namespace DCLF::Features::SwitchNodes
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Registered where Core's switch and toggle were, to keep the order.
			const auto switchNodes = a_context.RegisterSwitch({ .name = "CS_DCLF_SWITCH_NODES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: objects under switch nodes stay native (live toggle's seed)", .after = "CS_DCLF_PROJECTED_UV" });
			// A leaf under an NiSwitchNode, in the frames every switch on its path selects it (trees, harvestables): an object class.
			const auto toggle = a_context.RegisterToggle({ .name = "switch nodes", .label = "Under switch nodes: trees, harvestables",
				.seed = switchNodes, .entersClassification = true, .after = "refraction" });
			a_context.RegisterParent({ .parent = Api::EngineParent::Switch, .leaves = Api::ParentLeaves::SelectedLeaves, .toggle = toggle });
		}
	}

	Api::Feature Feature()
	{
		return { "switch-nodes", &Install };
	}
}

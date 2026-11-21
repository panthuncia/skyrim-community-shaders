// Switch nodes: the leaves under an NiSwitchNode DCLF draws while every switch on their path selects them.

#include "Features/DrawcallLimitFix/Features/SwitchNodes/SwitchNodes.h"

namespace DCLF::Features::SwitchNodes
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// A leaf under an NiSwitchNode, in the frames every switch on its path selects it (trees, harvestables).
			a_context.RegisterParent({ .parent = Api::EngineParent::Switch, .leaves = Api::ParentLeaves::SelectedLeaves });
		}
	}

	Api::Feature Feature()
	{
		return { "switch-nodes", &Install };
	}
}

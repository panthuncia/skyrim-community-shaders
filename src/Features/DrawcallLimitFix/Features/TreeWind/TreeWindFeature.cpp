// Tree wind: the trees DCLF owns, animated on the GPU.

#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeList.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWalk.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Features::TreeWind
{
	namespace
	{
		Api::SwitchId listAllSwitch;  // install

		void RegisterSettings(Api::InstallContext& a_context)
		{
			listAllSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_TREE_LIST_ALL", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Never,
				.description = "1 (T7f; off by default): the tree manager's list loop given DCLF's verdict for every BSTreeNode DCLF lists; off: the stood-in trees alone (T7b5's fix)",
				.after = "CS_DCLF_PLAIN_SNOW_SPARKLE" });
		}

		void Install(Api::InstallContext& a_context)
		{
			RegisterSettings(a_context);
			RegisterObjects(a_context);
			RegisterTables(a_context);
			RegisterSeeds(a_context);
			RegisterScene(a_context);
			RegisterPass(a_context);
			RegisterAnimation(a_context);
			RegisterReport(a_context);
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

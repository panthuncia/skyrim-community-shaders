// Hidden nodes feature registration.

#include "Features/DrawcallLimitFix/Features/HiddenNodes/HiddenNodes.h"

namespace DCLF::Features::HiddenNodes
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			RegisterWatch(a_context);
		}
	}

	Api::Feature Feature()
	{
		// Early hooks, because the kept scene lists and Features/Lod's gates check Scene::HiddenStoresLive when they install.
		return { "hidden-nodes", &Install, &InstallStores };
	}
}

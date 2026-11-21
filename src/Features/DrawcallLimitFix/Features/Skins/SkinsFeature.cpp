// Skins: the skinned shapes DCLF draws.

#include "Features/DrawcallLimitFix/Features/Skins/Skins.h"

namespace DCLF::Features::Skins
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Skinned });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::SkinPartitions });
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "skins", &Install };
	}
}

// Shadows/Engine: the shadow epoch's passes and frame points, the occlusion maps drawn in it (see Engine.h).

#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"

namespace DCLF::Features::Shadows::Engine
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// No setup, so their place among the registrations changes no order.
			RegisterPasses(a_context);
			RegisterShadowEpochs(a_context);
			InstallCasterHooks();
		}
	}

	Api::Feature Feature()
	{
		return { .name = "shadow-epochs", .install = &Install, .preflight = &PreflightCasterHooks };
	}
}

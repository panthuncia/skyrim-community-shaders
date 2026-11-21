// The Grass feature's entry. Registers the object class, capture sink, tables, passes, draw family and diagnostics.

#include "Features/DrawcallLimitFix/Features/Grass/Grass.h"

namespace DCLF::Features::Grass
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Grass });
			RegisterTables(a_context);
			RegisterPasses(a_context);
			RegisterFamily(a_context);
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		// Registered before any grass loads.
		return { .name = "grass", .install = &Install, .earlyHooks = &InstallHooks, .preflight = &PreflightHooks };
	}
}

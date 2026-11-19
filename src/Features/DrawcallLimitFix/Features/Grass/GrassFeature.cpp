// The Grass feature's entry. Registers the switch, toggle, capture sink, tables, passes, draw family and diagnostics.

#include "Features/DrawcallLimitFix/Features/Grass/Grass.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Grass
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Registration order sets the list order.
			const auto grass = a_context.RegisterSwitch({ .name = "CS_DCLF_GRASS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: grass stays native (Grass Optimizations' or the engine's) (live toggle's seed)", .after = "CS_DCLF_LOD_TREES" });
			// Grass is drawn from the mirror, not classified, so toggling drops no classification cache.
			const auto toggle = a_context.RegisterToggle({ .name = "grass", .label = "Grass", .seed = grass, .after = "tree LOD" });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Grass, .toggle = toggle });
			RegisterTables(a_context);
			RegisterPasses(a_context);
			RegisterFamily(a_context);
			RegisterReport(a_context);
			if (Diagnostics::Observe().scene)
				SubscribeParity();
		}
	}

	Api::Feature Feature()
	{
		// Registered before any grass loads, whatever the toggle, because a live toggle needs the mirror.
		return { .name = "grass", .install = &Install, .earlyHooks = &InstallHooks, .preflight = &PreflightHooks };
	}
}

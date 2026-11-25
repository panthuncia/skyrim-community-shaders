// Registers the billboards feature: the billboard parent.

#include "Features/DrawcallLimitFix/Features/Billboards/Billboards.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Features::Billboards
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Effect shapes must sit under exactly one billboard node. Particle systems can sit under any, because 0x140d3e650
			// runs each child's UpdateDownwardPass before ParticleSnapshots takes its snapshot.
			a_context.RegisterParent({ .parent = Api::EngineParent::Billboard, .leaves = Api::ParentLeaves::TurnedEffectShapes });
			a_context.RegisterParent({ .parent = Api::EngineParent::Billboard, .leaves = Api::ParentLeaves::ParticleSystems });
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "billboards", &Install };
	}
}

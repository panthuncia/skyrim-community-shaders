// Registers the billboards feature: its switch and the billboard parent.

#include "Features/DrawcallLimitFix/Features/Billboards/Billboards.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Features::Billboards
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			const auto particles = a_context.RegisterSwitch({ .name = "CS_DCLF_BILLBOARD_PARTICLES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on (T7f item 7): particle systems under NiBillboardNodes (the magic hands' fire cores and embers) drawn by DCLF's particle path like any other (their world and particles are their last update's); 0 they stay the engine's (billboard)",
				.after = "CS_DCLF_PLAIN_SNOW_SPARKLE" });
			// Effect shapes must sit under exactly one billboard node. Particle systems can sit under any, because 0x140d3e650
			// runs each child's UpdateDownwardPass before ParticleSnapshots takes its snapshot. The switch is read once, here.
			a_context.RegisterParent({ .parent = Api::EngineParent::Billboard, .leaves = Api::ParentLeaves::TurnedEffectShapes });
			if (SwitchValue(particles) != "0")
				a_context.RegisterParent({ .parent = Api::EngineParent::Billboard, .leaves = Api::ParentLeaves::ParticleSystems });
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "billboards", &Install };
	}
}

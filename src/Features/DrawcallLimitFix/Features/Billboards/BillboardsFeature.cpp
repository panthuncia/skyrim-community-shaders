// Registers the billboards feature: switches, toggle, the billboard parent and the parity's early hooks.

#include "Features/DrawcallLimitFix/Features/Billboards/Billboards.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Features::Billboards
{
	namespace
	{
		Api::ToggleId billboardsToggle;  // install

		void Install(Api::InstallContext& a_context)
		{
			const auto billboards = a_context.RegisterSwitch({ .name = "CS_DCLF_BILLBOARDS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "1: effect-shader meshes under an NiBillboardNode (fire cards, spray jets) drawn by DCLF, turned to each view's camera in the vertex stage as NiBillboardNode::OnVisible turns the node (needs CS_DCLF_EFFECTS); 0 they stay native (live toggle's seed)",
				.after = "CS_DCLF_REFRACTION" });
			const auto particles = a_context.RegisterSwitch({ .name = "CS_DCLF_BILLBOARD_PARTICLES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on (T7f item 7): particle systems under NiBillboardNodes (the magic hands' fire cores and embers) drawn by DCLF's particle path like any other (their world and particles are their last update's; needs CS_DCLF_BILLBOARDS and CS_DCLF_PARTICLES); 0 they stay the engine's (billboard)",
				.after = "CS_DCLF_OPAQUE_EFFECTS" });
			billboardsToggle = a_context.RegisterToggle({ .name = "billboards", .label = "Billboards: fire cards, spray jets",
				.tooltip = "DCLF draws effect-shader meshes under a billboard node, turning each to the drawing camera on the GPU as the engine's cull turns the node. On by default.",
				.seed = billboards, .entersClassification = true, .after = "refraction", .needs = { "effects" } });
			// Effect shapes must sit under exactly one billboard node. Particle systems can sit under any, because 0x140d3e650
			// runs each child's UpdateDownwardPass before ParticleSnapshots takes its snapshot. The switch is read once, here.
			a_context.RegisterParent({ .parent = Api::EngineParent::Billboard, .leaves = Api::ParentLeaves::TurnedEffectShapes, .toggle = billboardsToggle });
			if (SwitchValue(particles) != "0")
				a_context.RegisterParent({ .parent = Api::EngineParent::Billboard, .leaves = Api::ParentLeaves::ParticleSystems, .toggle = billboardsToggle });
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		// The parity observer must hook early, before the primary cull takes NiBillboardNode's OnVisible slot.
		return { "billboards", &Install, &InstallTurns };
	}

	Api::ToggleId Toggle()
	{
		return billboardsToggle;
	}
}

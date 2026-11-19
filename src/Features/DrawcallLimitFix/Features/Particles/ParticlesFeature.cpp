// Particles: the effect-shader particle systems DCLF draws from snapshots of the engine's particle update.

#include "Features/DrawcallLimitFix/Features/Particles/Particles.h"
#include "Features/DrawcallLimitFix/Features/Particles/MasterParticles.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Particles
{
	namespace
	{
		Api::ToggleId particlesToggle;  // install

		void Install(Api::InstallContext& a_context)
		{
			// Registered where Core's switch was, before Features/Billboards'.
			const auto particles = a_context.RegisterSwitch({ .name = "CS_DCLF_PARTICLES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "1: particle systems with an effect shader (NiParticleSystem: smoke, flames, glows) drawn by DCLF's forward epoch from snapshots taken at the engine's particle update, sorted with the blended objects (T7e; needs CS_DCLF_EFFECTS and CS_DCLF_FORWARD); 0 they stay native (live toggle's seed)",
				.after = "CS_DCLF_REFRACTION" });
			// On by default. CS_DCLF_PARTICLES=0 seeds it off. Needs effects.
			particlesToggle = a_context.RegisterToggle({ .name = "particles", .label = "Particle systems: smoke, flames, glows",
				.tooltip = "T7e: DCLF draws effect-shader particle systems in the forward epoch, sorted with the other blended objects, their quads built from snapshots taken at the engine's particle update. On by default (validated fp6).",
				.seed = particles, .entersClassification = true, .after = "refraction", .needs = { "effects", "forward" } });
			// A particle system's bound and quads change with each update, invisibly to the walk, so it is a mover placed every frame.
			a_context.RegisterGeometryType({ .geometryType = static_cast<std::uint8_t>(RE::BSGeometry::Type::kParticles), .toggle = particlesToggle,
				.itemClass = Api::FrameValueItemClass::ParticleSystem, .moves = true });
			a_context.RegisterGeometryType({ .geometryType = static_cast<std::uint8_t>(RE::BSGeometry::Type::kStripParticles), .toggle = particlesToggle });
			ParticleSnapshots::Get().Install(a_context);
			RegisterRows(a_context);
			MasterParticles::Install(a_context);
			RegisterReport(a_context);
			if (Diagnostics::Observe().claims)
				SubscribePasses();
		}
	}

	Api::Feature Feature()
	{
		return { .name = "particles", .install = &Install, .preflight = &MasterParticles::Preflight };
	}

	Api::ToggleId Toggle()
	{
		return particlesToggle;
	}
}

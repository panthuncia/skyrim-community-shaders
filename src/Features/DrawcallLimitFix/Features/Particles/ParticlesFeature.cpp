// Particles: the effect-shader particle systems DCLF draws from snapshots of the engine's particle update.

#include "Features/DrawcallLimitFix/Features/Particles/Particles.h"
#include "Features/DrawcallLimitFix/Features/Particles/MasterParticles.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Features::Particles
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Effect-shader particle systems, drawn in the forward epoch from snapshots taken at the engine's particle update.
			// A particle system's bound and quads change with each update, invisibly to the walk, so it is a mover placed every frame.
			a_context.RegisterGeometryType({ .geometryType = static_cast<std::uint8_t>(RE::BSGeometry::Type::kParticles), .ported = true,
				.itemClass = Api::FrameValueItemClass::ParticleSystem, .moves = true });
			a_context.RegisterGeometryType({ .geometryType = static_cast<std::uint8_t>(RE::BSGeometry::Type::kStripParticles), .ported = true });
			ParticleSnapshots::Get().Install(a_context);
			RegisterRows(a_context);
			MasterParticles::Install(a_context);
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { .name = "particles", .install = &Install, .preflight = &MasterParticles::Preflight };
	}
}

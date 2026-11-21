#pragma once

// Particles (dclf-architecture.md, "Particles"): DCLF draws NiParticleSystem geometry with a
// BSEffectShaderProperty in the forward epoch's sort, with quads built from snapshots taken at the engine's particle update.
// The feature registers the particle geometry types, the snapshot store and hooks (ParticleSnapshots.h), the palette rows class
// (ParticleRows.cpp) and its parity and report lines. Core keeps the item class, the deferral of its items in FrameValues' chunks,
// and the Effect port's particle eligibility. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Features/Particles/ParticleSnapshots.h"

#include <cstdint>
#include <string>

namespace DCLF::Features::Particles
{
	Api::Feature Feature();

	void RegisterReport(Api::InstallContext& a_context);

	void RegisterRows(Api::InstallContext& a_context);
}

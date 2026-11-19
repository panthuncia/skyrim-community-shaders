#pragma once

// Particles (CS_DCLF_PARTICLES; dclf-architecture.md, "Particles"): DCLF draws NiParticleSystem geometry with a
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
	Api::ToggleId Toggle();

	// CS_DCLF_SET_PARITY observer: a particle member's withheld blended pass, checked against the snapshots the producer expanded.
	void SubscribePasses();
	/** @brief Withheld particle passes since the last Take, those without a recent snapshot, and the first named. */
	struct PassCounts
	{
		std::uint64_t passes = 0, uncovered = 0;
		std::string first;
	};
	PassCounts TakePasses();

	void RegisterReport(Api::InstallContext& a_context);

	void RegisterRows(Api::InstallContext& a_context);
}

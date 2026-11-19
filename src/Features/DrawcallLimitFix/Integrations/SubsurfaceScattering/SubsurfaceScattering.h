#pragma once

// Subsurface Scattering's SetupSkin hook sets IsBeastRace for a face whose actor's race has its keyword. Lighting.hlsl reads it only under SKIN.
// It is per actor, so it goes in the record, not the permutation. DCLF reproduces it with a "beast race" reference fact,
// captured with the node's record, and an IsBeastRace Lighting object flag set per join.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::SubsurfaceScattering
{
	// Its object flag: IsBeastRace for a kFace or kFaceGenRGBTint face whose actor's race has the keyword.
	inline constexpr std::uint32_t kObjectBeastRace = 1u << 3;
	// Its record flag for IsBeastRace.
	inline constexpr std::uint32_t kRecordBeastRace = 1u << 0;

	Api::Feature Integration();
}

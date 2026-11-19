#pragma once

// Physical Sky's LUTs as frame textures: transmittance (t61), sky-view (t62), aerial perspective (t63) and disk phase (t106).
// Registered so the reflection's faces, drawn before the main pass captures, read them too. Its other registers are still captured.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::PhysicalSky
{
	inline constexpr std::uint32_t kTransmittanceRegister = 61;
	inline constexpr std::uint32_t kSkyViewRegister = 62;
	inline constexpr std::uint32_t kAerialPerspectiveRegister = 63;
	inline constexpr std::uint32_t kDiskPhaseRegister = 106;

	Api::Feature Integration();
}

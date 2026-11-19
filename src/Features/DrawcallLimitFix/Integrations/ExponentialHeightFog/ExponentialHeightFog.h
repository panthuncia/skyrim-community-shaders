#pragma once

// Exponential Height Fog's light scattering volumes (t19 near, t22 far) as frame textures, bound in its Prepass.
// Registered so the reflection's faces read them too, and so the colour passes read the graph volumes on the ORG path.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::ExponentialHeightFog
{
	// t19: the integrated light scattering volume.
	inline constexpr std::uint32_t kLightScatteringRegister = 19;
	// t22: the far volume.
	inline constexpr std::uint32_t kLightScatteringFarRegister = 22;

	Api::Feature Integration();
}

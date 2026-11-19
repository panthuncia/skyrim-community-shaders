#pragma once

// Volumetric Shadows' downsampled sun shadow map (t18) as a frame texture, bound at the engine's shadow mask draw.
// Registered so the reflection's faces, drawn before the main pass captures, read it too.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::VolumetricShadows
{
	// t18: the shared shadow map.
	inline constexpr std::uint32_t kSharedShadowMapRegister = 18;

	Api::Feature Integration();
}

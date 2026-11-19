#pragma once

// Terrain Shadows' shadow height map (t60) as a frame texture, bound in its early prepass after the shadow maps.
// Registered so the reflection's faces, drawn before the main pass captures, read it too.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::TerrainShadows
{
	// t60: the shadow height map.
	inline constexpr std::uint32_t kShadowHeightRegister = 60;

	Api::Feature Integration();
}

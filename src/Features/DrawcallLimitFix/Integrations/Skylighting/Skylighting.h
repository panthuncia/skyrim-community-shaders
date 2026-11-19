#pragma once

// Skylighting's probe array (t50) and shadow visibility (t53) as frame textures, bound in its prepass.
// Registered so the reflection's faces, drawn before the main pass captures, read them too. Its occlusion map lives in Features/OcclusionMaps.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::Skylighting
{
	// t50 and t53: the probe array and the shadow visibility.
	inline constexpr std::uint32_t kProbeArrayRegister = 50;
	inline constexpr std::uint32_t kShadowVisibilityRegister = 53;

	Api::Feature Integration();
}

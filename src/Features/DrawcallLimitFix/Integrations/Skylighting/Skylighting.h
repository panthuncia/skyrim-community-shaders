#pragma once

// Skylighting's probe array (t50), roof coverage (t51, t52) and shadow visibility (t53) as frame textures, bound in its prepass.
// Registered so the reflection's faces, drawn before the main pass captures, read them too. Its occlusion map lives in Features/OcclusionMaps.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::Skylighting
{
	// t50 to t53: the probe array, the probe coverage, the zenith capture and the shadow visibility.
	inline constexpr std::uint32_t kProbeArrayRegister = 50;
	inline constexpr std::uint32_t kCoverageRegister = 51;
	inline constexpr std::uint32_t kZenithRegister = 52;
	inline constexpr std::uint32_t kShadowVisibilityRegister = 53;

	Api::Feature Integration();
}

#pragma once

// Cloud Shadows' occlusion cubemap (t25) and self-shadowing input (t26) as frame textures.
// CS binds t25 in the water reflection's prepass, so the reflection's faces, drawn before the main pass captures, need it registered too.
// t26 is registered for its graph path only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::CloudShadows
{
	// t25: the cloud occlusion cubemap.
	inline constexpr std::uint32_t kCloudOcclusionRegister = 25;
	// t26: the cloud self-shadowing input.
	inline constexpr std::uint32_t kSelfShadowRegister = 26;

	Api::Feature Integration();
}

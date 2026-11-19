#pragma once

// Screen Space Shadows' shadow texture (t45) as a frame texture, for its graph path only.
// The colour commits keep the captured view, and on the ORG path read the published texture instead.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::ScreenSpaceShadows
{
	// t45: the shadow texture.
	inline constexpr std::uint32_t kShadowTextureRegister = 45;

	Api::Feature Integration();
}

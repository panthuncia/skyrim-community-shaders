#pragma once

// Water Effects' caustics texture (t65) as a frame texture, bound in its prepass.
// Registered so the reflection's faces, drawn before the main pass captures, read it too.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::WaterEffects
{
	// t65: the caustics texture.
	inline constexpr std::uint32_t kCausticsRegister = 65;

	Api::Feature Integration();
}

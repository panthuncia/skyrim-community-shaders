#pragma once

// Image Based Lighting's environment and sky SH (t76, t77) and static cubemaps (t78, t79) as frame textures.
// The water reflection's prepass binds them too, so the reflection's faces, drawn before the main pass captures, need them registered.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::IBL
{
	// t76-t79: the environment SH, the sky SH, and the static diffuse and specular cubemaps.
	inline constexpr std::uint32_t kEnvironmentRegister = 76;
	inline constexpr std::uint32_t kSkyRegister = 77;
	inline constexpr std::uint32_t kStaticDiffuseRegister = 78;
	inline constexpr std::uint32_t kStaticSpecularRegister = 79;

	Api::Feature Integration();
}

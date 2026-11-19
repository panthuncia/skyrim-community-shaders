#pragma once

// Dynamic Cubemaps' reflections and environment cubemaps (t30, t31) as frame textures, bound after the deferred pass for the next frame.
// Registered so the reflection's faces, drawn before the main pass captures, read them too.
// t30's view is the one PostDeferred bound, because the reflection state it was chosen by is reset at every Present.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::DynamicCubemaps
{
	// t31: the environment cubemap.
	inline constexpr std::uint32_t kReflectionsRegister = 30;
	inline constexpr std::uint32_t kEnvironmentRegister = 31;

	Api::Feature Integration();
}

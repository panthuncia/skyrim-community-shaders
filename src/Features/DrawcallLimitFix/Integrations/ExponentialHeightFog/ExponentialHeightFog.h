#pragma once

// Exponential Height Fog (Features/ExponentialHeightFog.h): its integrated light scattering volumes (t19 near, t22 far), which CS binds
// in its Prepass (BindIntegratedLightScattering) and every colour draw reads. The integration registers them frame textures
// (Api::FrameTexture, with capturedWhenNull), so the views with frame constants of their own (the reflection's faces), drawn before the
// main pass captures its frame, read them too, and the colour passes read the published graph volumes on the ORG path. Included by the integration's sources and Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::ExponentialHeightFog
{
	// t19: the integrated light scattering volume, a frame texture (Features/Manifest.cpp).
	inline constexpr std::uint32_t kLightScatteringRegister = 19;
	// t22: the far volume's, a frame texture (Features/Manifest.cpp).
	inline constexpr std::uint32_t kLightScatteringFarRegister = 22;

	// ExponentialHeightFogIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}

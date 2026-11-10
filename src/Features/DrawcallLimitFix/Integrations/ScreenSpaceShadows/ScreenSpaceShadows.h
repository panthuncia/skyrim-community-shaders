#pragma once

// Screen Space Shadows (Features/ScreenSpaceShadows.h): its shadow texture (t45, ScreenSpaceShadows.hlsli's ScreenSpaceShadowsTexture),
// which CS binds in its prepass and every colour draw reads. The integration registers it a frame texture (Api::FrameTexture, with
// capturedWhenNull) for its graph path only: the colour commits keep the captured view, and on the ORG path
// (CS_ORG_SCREEN_SPACE_SHADOWS) they read the published texture instead. Included by the integration's sources and
// Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::ScreenSpaceShadows
{
	// t45: the shadow texture, a frame texture (Features/Manifest.cpp).
	inline constexpr std::uint32_t kShadowTextureRegister = 45;

	// ScreenSpaceShadowsIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}

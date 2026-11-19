#pragma once

// LocalLights: which local shadow lights draw, and which objects receive them.
// LightSelection ports the engine's shadow light selection and tests rooms through Portals.
// LocalShadowLights gives each DCLF object its main-pass shadow mask.
// LightContextResources publishes the selected lights' portal contexts to the GPU.
// The caster side is in Shadows/Local. Core keeps the shared shadow engine.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Features::LocalLights
{
	Api::Feature Feature();

	// Objects with these flags only take lights that affect land (+0x61). kLandscape is 14, kNoLODLandFade 46 (FUN_14147c340).
	inline constexpr std::uint64_t kLandscapeFlags = (1ull << 14) | (1ull << 46);

	void LightSelectionLines();
}

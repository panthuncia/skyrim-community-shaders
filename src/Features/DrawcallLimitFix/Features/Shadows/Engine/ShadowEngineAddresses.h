#pragma once

// Engine addresses only Shadows/Engine reads. Shared ones stay in Core/Engine/EngineAccess.h. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <cstdint>

namespace DCLF::Features::Shadows::Engine::Address
{
	// The shadow lights' selection. Each call site is checked with CallsTo before it is patched.
	inline constexpr DCLF::Engine::Offset kCalculateAndDrawShadowCasterLights{ { 107132 }, { 100414 } };  // NiCamera::CalculateAndDrawShadowCasterLights
	inline constexpr DCLF::Engine::Offset kCastersCallSite{ { 36559, 0x1c4 }, { 35560, 0x1c4 } };                      // Main::Draw's call of it (Main::Draw +0x1C4)
	inline constexpr DCLF::Engine::Offset kKeepLight{ { 106365 }, { 99728 } };
	// The focus accumulation (AE FUN_1414f0b90, SE 0x1413054c0) registers each focus target's subtree whole into the light's focus
	// accumulator. For a spot host it runs before the light is kept, for the sun after its Accumulate.
	inline constexpr DCLF::Engine::Offset kFocusAccumulation{ { 107603 }, { 100819 } };                           // FUN_1414a3fb0(node, light, first slot, slots)
	inline constexpr DCLF::Engine::Offset kKeepSunCallSite{ { 107132, 0xd6 }, { 100418, 0xbd } };                     // the sun's, in CalculateAndDrawShadowCasterLights (SE: in its sun part, FUN_1412e2e10)
	inline constexpr DCLF::Engine::Offset kKeepLightCallSite{ { 107137, 0x271 }, { 100419, 0x271 }, { 107137, 0x251 } };                   // each kept light's, in CalculateActiveShadowCasterLights
}

#pragma once

// Terrain Blending: while loaded and enabled it holds the terrain back, replaying every kMultiTextureLandscape pass after the opaque pass
// blended over what lies beneath, then the opted-out meshes marked kNoTransparencyMultiSample with an EQUAL test.
// The integration registers what DCLF needs to draw both the same way: a classify input, a switch and toggle, gates on the terrain techniques,
// two held groups, its mask (t55), the scene depth (t17) for its graph path, and a near depth twin.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::TerrainBlending
{
	// Its held groups' slots: the terrain blocks in group 1, then the no-blend meshes in group 2.
	inline constexpr Api::MainRangeSlot kTerrainRange = Api::MainRangeSlot::Appended0;
	inline constexpr Api::MainRangeSlot kNoBlendRange = Api::MainRangeSlot::Appended1;
	// t55: Terrain Blending's mask.
	inline constexpr std::uint32_t kMaskRegister = 55;
	// t17: the scene depth, which is Terrain Blending's 16-bit blended depth while it blends. A frame texture for its graph path only.
	inline constexpr std::uint32_t kSceneDepthRegister = 17;

	Api::Feature Integration();
}

#pragma once

// Object and terrain LOD, drawn by DCLF from the terrain manager's blocks under TES::lodLandRoot.
// Object LOD is a BSSubIndexTriShape drawn by its visible segments (LodSegments.h). Terrain LOD is a land block's shape.
// The LOD gates (LodGates.h, LodGateComponent.h) hold each engine LOD swap until DCLF's claims cover the incoming level.
// Core still owns the LOD fades and the IsLodLand and IsLodObject predicates. See dclf-architecture.md, "LOD".

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Lod
{
	// Drawn by a view's faces in the engine's place while both classes are on and the swaps are gated.
	inline constexpr Api::ViewRoot kOwnedRoot = Api::ViewRoot::LodLandAndObjects;

	Api::Feature Feature();
}

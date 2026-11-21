#pragma once

// Billboards: effect-shader meshes under an NiBillboardNode, which the vertex stage turns to each view's camera
// (Effect.hlsl, DCLFBillboardWorld). Particle systems under billboard nodes go through DCLF's particle path.
// Core keeps the placement row and face mode (Core/Gpu/PlacementRows/BillboardRow.h).
// See dclf-architecture.md, "Billboards and aligned particles".

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Billboards
{
	Api::Feature Feature();

	void RegisterReport(Api::InstallContext& a_context);
}

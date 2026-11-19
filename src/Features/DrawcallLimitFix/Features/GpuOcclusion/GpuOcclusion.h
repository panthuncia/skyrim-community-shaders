#pragma once

// GpuOcclusion builds the depth pyramid (HZB) that the two-phase occlusion test reads (Api/DepthPyramid.h).
// The pass runs in the depth epoch, between phase 1's depth draw and phase 2's retest. Core owns the test itself.
// Without this feature, or if its program fails, culling is frustum-only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::GpuOcclusion
{
	Api::Feature Feature();

	void RegisterPyramid(Api::InstallContext& a_context);
}

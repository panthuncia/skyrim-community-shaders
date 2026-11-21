#pragma once

// Landscape: DCLF draws terrain, the MTLand and MTLandLODBlend Lighting techniques (8, 19).
// Core owns the shared land material handling. LOD terrain belongs to Features/Lod.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Landscape
{
	Api::Feature Feature();
	/** @brief Registers per-quad landscape seam resources for the batched terrain shaders. */
	void InstallSeams(Api::InstallContext& a_context);
}

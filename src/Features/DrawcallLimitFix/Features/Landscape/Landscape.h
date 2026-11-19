#pragma once

// Landscape (CS_DCLF_MTLAND): DCLF draws terrain, the MTLand and MTLandLODBlend Lighting techniques (8, 19).
// While Terrain Blending holds the terrain, its integration also gates both techniques on its own toggle.
// Core owns the shared land material handling. LOD terrain belongs to Features/Lod.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Landscape
{
	Api::Feature Feature();
	/** @brief Registers per-quad landscape seam resources for the batched terrain shaders. */
	void InstallSeams(Api::InstallContext& a_context);

	// CS_DCLF_LAND_TRACE=1. Main thread, at the scene frame's start, with or without DCLF.
	void SubscribeLandTrace();
}

#pragma once

// NPC faces (dclf-architecture.md, "NPC faces: positions published by the engine's own writer").
// DCLF never reads face positions from the shape itself. Hooks in the engine's morph job publish snapshots (FaceSnapshots),
// a coordinator component tracks the face shapes (FaceShapes.cpp), and a vertex stream uploads the positions (FacePositions.cpp).

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Faces
{
	Api::Feature Feature();
	// a_taken says whether the face objects are DCLF's.
	void RegisterShapes(Api::InstallContext& a_context, bool a_taken);
	// Register after the component.
	void RegisterPositions(Api::InstallContext& a_context);
}

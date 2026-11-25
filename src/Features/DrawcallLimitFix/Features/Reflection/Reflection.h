#pragma once

// Reflection (dclf-architecture.md, "Water reflections"): DCLF draws the water reflection's cube map faces as
// a view of the one scene. The feature registers the view's contribution and owns its engine hooks (ReflectionFaces), its per-frame
// work (ReflectionView, ReflectionEpoch) and its diagnostics.
// Core keeps what every bucketed view shares: the epoch's passes, slot state, target import, PassCapture withholding, tree LOD per
// face and the cube camera's LOD roots. The feature reaches those only through the view engine's calls, never a LOD feature's headers.
// Included by the feature's sources only, and by Features/Manifest.cpp for the constants.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <string>

namespace DCLF::ShadowCameras
{
	struct Camera;
}

namespace DCLF::Features::Reflection
{
	// Features/Manifest.cpp checks these against Core's.
	inline constexpr std::uint8_t kSetReflection = 1u << 5;
	inline constexpr std::uint32_t kViewReflection = 1u << 7;
	inline constexpr std::uint32_t kFaces = 6;

	Api::Feature Feature();

	// The view's work after tree LOD's decision, its coverage, and the face hooks' callbacks.
	void Prepare(const Api::ViewPointContext& a_context);
	bool Covered(const Api::ViewCoverage& a_coverage);
	// TESWaterReflections::Update hides the player's third-person 3D and the water shapes around the cube faces.
	void CullHiddenByUpdate(Api::CullHiddenNodes& a_nodes);
	void FaceOriented(const ShadowCameras::Camera& a_camera, std::uint32_t a_face);

	// The faces' commit, inside the depth commit, and the report lines.
	void Execute(const Api::ViewPointContext& a_context);
	std::string Report();
}

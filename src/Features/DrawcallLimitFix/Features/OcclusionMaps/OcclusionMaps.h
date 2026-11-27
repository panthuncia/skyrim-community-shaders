#pragma once

// OcclusionMaps: Skylighting's sky occlusion map and the precipitation occlusion mask
// (render mode 0x1C), drawn in the shadow epoch in the engine's place.
// With Skylighting loaded it drives both maps. Without it the shell drives the mask (RenderPrecipitationAtShadowMaps).
// The feature supplies each map's contribution: occluder rules, size test, target, camera and parity.
// Core keeps the maps' part of the shadow commit, their slots, targets and the walk that hands objects to the rules.
// Included by the feature's sources only, and by Features/Manifest.cpp for the constants.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Api/ShadowViews.h"

#include <cstdint>

namespace DCLF::Features::OcclusionMaps
{
	// Features/Manifest.cpp checks these against Core's. There is no HLSL mirror, since the shadow builds take a map's view bit as data.
	inline constexpr std::uint8_t kSetOccluderSky = 1u << 2;            // Skylighting's occlusion map
	inline constexpr std::uint8_t kSetOccluderPrecipitation = 1u << 3;  // the precipitation occlusion mask
	inline constexpr std::uint32_t kViewSkyOccluder = 1u << 5;
	inline constexpr std::uint32_t kViewPrecipOccluder = 1u << 6;

	Api::Feature Feature();

	// The maps' occluder rules, pure per walked entry. Each copies its engine or Skylighting technique's filter.
	std::uint32_t SkylightingTechnique(const Api::OccluderLeaf& a_leaf);
	std::uint32_t PrecipitationTechnique(const Api::OccluderLeaf& a_leaf);
	std::uint32_t EnginePrecipitationTechnique(const Api::OccluderLeaf& a_leaf);
	// Guest: both maps use the precipitation accumulator's camera, which ComputeProjection set up for the map being drawn.
	// Native: the mask's camera from the capture, the sky map's from Skylighting (Snapshot::PrecipitationOcclusion).
	void PrecipitationCamera(Api::OcclusionCamera& a_camera);
	void SkyCamera(Api::OcclusionCamera& a_camera);

	void RegisterReport(Api::InstallContext& a_context);
}

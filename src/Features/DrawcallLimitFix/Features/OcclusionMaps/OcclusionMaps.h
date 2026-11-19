#pragma once

// OcclusionMaps (CS_DCLF_SKYLIGHT, CS_DCLF_PRECIPITATION): Skylighting's sky occlusion map and the precipitation occlusion mask
// (render mode 0x1C), drawn in the shadow epoch in the engine's place while the map's toggle is on.
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
	// Both maps use the precipitation accumulator's camera. It draws Skylighting's map while Skylighting draws it, else the mask.
	void PrecipitationCamera(Api::OcclusionCamera& a_camera);
	bool SkyOwnsAccumulator(const Api::OcclusionAccumulator& a_accumulator);
	bool PrecipitationOwnsAccumulator(const Api::OcclusionAccumulator& a_accumulator);

	void RegisterReport(Api::InstallContext& a_context);

	// CS_DCLF_SKYLIGHT_PARITY: every 120th frame a map is rendered both ways, engine first, and compared texel by texel.
	void RegisterParity(Api::InstallContext& a_context);
	// Whether this frame renders a_map's reference. Render thread.
	bool ReferenceDue(std::uint32_t a_map);

	/** @brief Subscribes the check of a capture against its map's own camera. */
	void SubscribeOcclusionViewCheck();
}

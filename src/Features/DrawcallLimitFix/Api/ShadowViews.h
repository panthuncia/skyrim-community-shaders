#pragma once

// Occlusion maps, shadow sources and shadow entry exclusions. Core has one shadow engine that draws all of them.
// Casters are keyed by the engine's render mode, not by source, so sources own no phase or view bit.

#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Api/ViewCommon.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/ShadowViews.h.
	class OcclusionTargetImport;
	class OccluderLeaf;
	class OcclusionCamera;
	class ShadowLightKept;
	class ShadowSourceCamera;
	// Defined in Core/Render/Shadow/ShadowExclusionBuild.h.
	class ShadowExclusionBuild;

	/** @brief A view of the precipitation accumulator, render mode 0x1C. The order is Core's slot order. */
	enum class OcclusionMap : std::uint8_t
	{
		Sky,
		Precipitation,
		Count,
	};
	/** @brief Whether a map draws into the engine's precipitation mask or into a texture the feature imports. */
	enum class OcclusionTarget : std::uint8_t
	{
		EngineMask,
		ImportedTexture,
	};
	/** @brief Names for a map's parity list, report lines and imported target. */
	struct OcclusionLabels
	{
		std::string_view list, report, target;
	};
	/**
	 * @brief An occlusion map, DCLF's while `available` is set.
	 *
	 * - technique: pure, per walked non-LOD Lighting object. Returns the Utility technique to draw it with, or 0 if it occludes nothing.
	 *   LOD never occludes, because the engine's mask cull does not visit the LOD root.
	 * - minRadius: skip objects with a bound radius of 32 or less, matching Skylighting's size test.
	 * - floorHeight: optional. Skip objects wholly below this height, since they cannot block any probe's view of the sky.
	 * - importTarget: called once, Precipitation first, then Sky.
	 */
	struct OcclusionMapContribution
	{
		OcclusionMap map = OcclusionMap::Count;
		const bool* available = nullptr;
		ViewBits bits;
		std::uint32_t (*technique)(const OccluderLeaf& a_leaf) = nullptr;
		bool minRadius = false;
		float (*floorHeight)() = nullptr;
		OcclusionTarget target = OcclusionTarget::EngineMask;
		OcclusionLabels labels;
		void (*importTarget)(OcclusionTargetImport& a_import) = nullptr;
		void (*camera)(OcclusionCamera& a_camera) = nullptr;
	};

	/** @brief The lights whose views the shadow engine draws. Sun and Spot share the clamped render mode. Point is the paraboloid. */
	enum class ShadowSource : std::uint8_t
	{
		Sun,
		Spot,
		Point,
		Focus,
		Count,
	};
	/**
	 * @brief A shadow source. Views of a source nobody registered stay with the engine.
	 * The feature computes the cameras itself, using Core's camera math in Core/Render/Shadow/ShadowCameras.h.
	 *
	 * - camera: render thread, per view. Leaves the camera null when the source has none this frame.
	 * - lightKept: Spot and Point only. Called once per kept light at the end of light selection.
	 * - fullFrustumWindowEnd: Sun only. Returns whether this frame's full-frustum cull applied the source's exclusion.
	 */
	struct ShadowSourceContribution
	{
		ShadowSource source = ShadowSource::Count;
		void (*camera)(ShadowSourceCamera& a_camera) = nullptr;
		void (*lightKept)(const ShadowLightKept& a_light) = nullptr;
		bool (*fullFrustumWindowEnd)() = nullptr;
		void (*frameStart)(std::uint32_t a_frame) = nullptr;
		void (*frameEnd)() = nullptr;
	};

	/**
	 * @brief The sun's pending exclusion, as the primary cull's scene-list filter reads it.
	 * A root leaves the lists only if its entry also leaves the sun's cascades. An empty `held` means none is pending.
	 *
	 * - excluded: one byte per candidate, nonzero when the entry is taken out of the cascade culls.
	 * - version: changes only when excluded does.
	 */
	struct ShadowListExclusion
	{
		std::shared_ptr<const void> held;
		const void* candidates = nullptr;
		std::span<const std::uint8_t> excluded;
		std::uint64_t version = 0;
	};

	/**
	 * @brief The shadow entry exclusions: which entries DCLF can take out of the engine's sun and point light culls next frame.
	 * The exclusion type is the feature's own, and Core holds it opaquely. Without this contribution nothing is excluded.
	 *
	 * - build: runs once per used mode in the shadow payload build, Sun then Point.
	 * - publish: render thread, once per frame after the shadow epoch drew. Never before DCLF's first covered frame.
	 *   Claims are never withdrawn once installed.
	 * - listExclusion: fills a_view with the Sun's pending exclusion, or leaves it empty.
	 */
	struct ShadowExclusionContribution
	{
		std::string_view name;
		void (*build)(ShadowExclusionBuild& a_build) = nullptr;
		void (*publish)(ShadowSource a_source, std::shared_ptr<void> a_exclusion) = nullptr;
		void (*frameStart)() = nullptr;
		void (*listExclusion)(ShadowListExclusion& a_view) = nullptr;
	};

	/**
	 * @brief Passed to Feature::sourceHooks for each source in turn.
	 * Point culls need bits from the Sun's registration hooks, so they must install after them.
	 */
	class ShadowSourceHooks
	{
	public:
		ShadowSource source = ShadowSource::Count;
		/** @brief Main thread. Reports that the source's cull hooks are installed. */
		void CullsInstalled();
	};
}

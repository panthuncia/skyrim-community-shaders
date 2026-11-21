#pragma once

// Core's side of Api/ShadowViews.h. Occlusion map, shadow source and exclusion entries are resolved once at install.
// The shadow engine reads them, and what it derives from them, as plain data on its per-object and per-frame paths.

#include "Features/DrawcallLimitFix/Api/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

struct ID3D11Texture2D;
struct ID3D11DepthStencilView;

namespace org
{
	class ExternalTextureResource;
}

namespace RE
{
	class BSShadowLight;
	class NiAVObject;
	class NiCamera;
	class BSShaderAccumulator;
}

namespace DCLF::ShadowCameras
{
	struct Camera;
}

namespace DCLF::Api
{
	/**
	 * @brief What importTarget gets for an ImportedTexture map: the texture and depth view to import, and whether they were supplied.
	 * A graph-owned target also gives its graph resource, which the shadow epoch declares instead of importing again.
	 */
	class OcclusionTargetImport
	{
	public:
		bool supplied = false;
		ID3D11Texture2D* texture = nullptr;
		ID3D11DepthStencilView* view = nullptr;
		std::shared_ptr<org::ExternalTextureResource> graph;
	};

	/** @brief What a map's technique reads of the walked object. The nearest fade node's BSX flags are only walked when asked for. */
	class OccluderLeaf
	{
	public:
		std::uint64_t flags = 0;    // BSShaderProperty::flags
		bool alphaTesting = false;  // NiAlphaProperty::GetAlphaTesting
		bool referenced = false;    // its node has a reference
		/** @brief BSX flags of the nearest fade node above the object, or -1. */
		std::int32_t NearestFadeNodeBsx() const { return nearestFadeBsx(*this); }

		// What NearestFadeNodeBsx walks.
		const void* scene = nullptr;
		const void* records = nullptr;
		std::int32_t (*nearestFadeBsx)(const OccluderLeaf& a_leaf) = nullptr;
	};

	/** @brief What a map's camera gets. Leave it empty when the view is not set up. */
	class OcclusionCamera
	{
	public:
		const RE::NiCamera* camera = nullptr;
	};

	/** @brief A light the selection kept, and the scene frame of the selection. */
	class ShadowLightKept
	{
	public:
		const RE::BSShadowLight* light = nullptr;
		std::uint32_t sceneFrame = 0;
	};

	/**
	 * @brief What a source's camera gets: a view's light, descriptor and scene frame.
	 * It answers with DCLF's camera, or null, and for a focus view the target's node.
	 */
	class ShadowSourceCamera
	{
	public:
		const RE::BSShadowLight* light = nullptr;
		std::uint32_t descriptor = 0;
		std::uint32_t sceneFrame = 0;
		const ShadowCameras::Camera* camera = nullptr;
		const RE::NiAVObject* focusTarget = nullptr;
	};
}

namespace DCLF::Extensions
{
	inline constexpr std::size_t kOcclusionMaps = static_cast<std::size_t>(Api::OcclusionMap::Count);
	inline constexpr std::size_t kShadowSources = static_cast<std::size_t>(Api::ShadowSource::Count);
	// Shadow engine modes are plain, clamped and paraboloid, then one per map. Both counts are checked where they are defined.
	inline constexpr std::size_t kShadowEngineModes = 3 + kOcclusionMaps;
	inline constexpr std::size_t kShadowViewKinds = 4;

	/** @brief A map's resolved entry. Every field keeps its default when no feature registered it. */
	struct OcclusionMapEntry
	{
		bool registered = false;
		const bool* available = nullptr;              // read whenever the map is
		std::uint8_t phase = 0;
		std::uint32_t viewBit = 0;
		std::uint32_t (*technique)(const Api::OccluderLeaf&) = nullptr;  // per walked entry
		bool minRadius = false;                       // kCullMinRadius
		float (*floorHeight)() = nullptr;             // null means no floor
		Api::OcclusionTarget target = Api::OcclusionTarget::EngineMask;
		std::string list, report, targetName;         // owned here
		void (*importTarget)(Api::OcclusionTargetImport&) = nullptr;
		void (*camera)(Api::OcclusionCamera&) = nullptr;
	};
	struct ShadowSourceEntry
	{
		bool registered = false;
		void (*camera)(Api::ShadowSourceCamera&) = nullptr;
		void (*lightKept)(const Api::ShadowLightKept&) = nullptr;
		bool (*fullFrustumWindowEnd)() = nullptr;  // Sun only
		void (*frameStart)(std::uint32_t) = nullptr;
		void (*frameEnd)() = nullptr;
	};
	struct ShadowViewSet
	{
		std::array<OcclusionMapEntry, kOcclusionMaps> maps;
		std::array<ShadowSourceEntry, kShadowSources> sources;
		// Null means no exclusion.
		void (*exclusion)(Api::ShadowExclusionBuild&) = nullptr;
		void (*exclusionPublish)(Api::ShadowSource, std::shared_ptr<void>) = nullptr;
		void (*exclusionFrameStart)() = nullptr;
		void (*listExclusion)(Api::ShadowListExclusion&) = nullptr;
		std::uint8_t occluderPhases = 0;
		std::uint32_t occluderViewBits = 0;
		// Each mode's set phase. 0 for a map nobody takes.
		std::array<std::uint8_t, kShadowEngineModes> phaseOfMode{};
		// The phases the set decision tests readiness for, in order: casters first, then the registered maps in slot order.
		std::array<std::uint8_t, 2 + kOcclusionMaps> readinessOrder{};
		std::uint32_t readinessCount = 0;
		// The source an engine view draws for, by kind and whether it is a focus view. Count means DCLF has no camera for it.
		std::array<std::array<Api::ShadowSource, 2>, kShadowViewKinds> sourceOf{};
		// The order map targets are imported in: the precipitation mask, then the sky map.
		std::array<Api::OcclusionMap, kOcclusionMaps> importOrder{};
	};

	namespace Detail
	{
		// Written only by ResolveShadowViews on the main thread before DCLF's threads exist. Read-only after, so plain reads are safe.
		inline ShadowViewSet resolvedShadowViews;
	}

	/** @brief Any thread, after InstallFeatures. */
	inline const ShadowViewSet& ShadowViewsSet() { return Detail::resolvedShadowViews; }
	/** @brief Any thread, after InstallFeatures. */
	inline const OcclusionMapEntry& OcclusionMapOf(std::uint32_t a_map) { return ShadowViewsSet().maps[a_map]; }
	/** @brief Any thread, after InstallFeatures. */
	inline const ShadowSourceEntry& ShadowSourceOf(Api::ShadowSource a_source) { return ShadowViewsSet().sources[static_cast<std::size_t>(a_source)]; }
	/** @brief Render thread. a_source's camera for a_request's view, or null for an unregistered source. */
	inline const ShadowCameras::Camera* SourceCamera(Api::ShadowSource a_source, Api::ShadowSourceCamera& a_request)
	{
		if (const auto& source = ShadowSourceOf(a_source); source.camera)
			source.camera(a_request);
		return a_request.camera;
	}
	/** @brief Snapshot builder. Runs the exclusion contribution's build, if any. */
	inline void BuildShadowExclusion(Api::ShadowExclusionBuild& a_build)
	{
		if (const auto exclusion = ShadowViewsSet().exclusion)
			exclusion(a_build);
	}
	/** @brief Render thread, after the shadow epoch drew. Hands a_source's exclusion back to the contribution, if any. */
	inline void PublishShadowExclusion(Api::ShadowSource a_source, std::shared_ptr<void> a_exclusion)
	{
		if (const auto publish = ShadowViewsSet().exclusionPublish)
			publish(a_source, std::move(a_exclusion));
	}
	/** @brief Render thread, once per frame. Runs the exclusion contribution's frameStart, if any. */
	inline void ShadowExclusionFrameStart()
	{
		if (const auto frameStart = ShadowViewsSet().exclusionFrameStart)
			frameStart();
	}
	/** @brief Render thread. The Sun's pending exclusion for the scene-list filter, or left empty. */
	inline void ListExclusion(Api::ShadowListExclusion& a_view)
	{
		if (const auto listExclusion = ShadowViewsSet().listExclusion)
			listExclusion(a_view);
	}
	/** @brief Main thread, once, after the features' installs. Builds the entries from the registered contributions. */
	void ResolveShadowViews(std::span<const Api::OcclusionMapContribution, kOcclusionMaps> a_maps,
		std::span<const Api::ShadowSourceContribution, kShadowSources> a_sources, const Api::ShadowExclusionContribution& a_exclusion);
	/** @brief Whether a_source's feature installed its engine cull hooks. Point culls must install after the Sun's. */
	bool SourceCullsInstalled(Api::ShadowSource a_source);
	/** @brief Render thread, once per frame. Each registered source's frameStart, in source order. */
	void ShadowSourcesFrameStart(std::uint32_t a_frame);
	/** @brief Render thread, at Present. Each registered source's frameEnd, in source order. */
	void ShadowSourcesFrameEnd();

	/**
	 * @brief Whether DCLF draws occlusion map a_map: taken and available.
	 * The available flag comes from Skylighting, whose hook drives both maps.
	 */
	inline bool OcclusionOn(std::uint32_t a_map)
	{
		if (a_map >= kOcclusionMaps)
			return false;
		const auto& map = OcclusionMapOf(a_map);
		return map.registered && *map.available;
	}
}

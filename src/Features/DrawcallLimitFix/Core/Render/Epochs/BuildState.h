#pragma once

// What DCLF's epoch graphs are built from (BuildState), as one immutable value.
//
// The render thread makes a new value when its live state changes and posts it with the revision inputs. Builds run on
// the scene worker while the render thread keeps changing, so a build reads only this value, never the live resources.
// Finished builds are staged. Once every one is, the value is the built one, and snapshots are made for it. The value is
// the attached one from the Begin that adopts a snapshot made for it, with the staged builds.
//
// Per-frame fields stay on the live resources, since only a pass's preparation or recording reads them. A pass takes the
// conditions it needs from the state it was made with, never from the attached graphs' ones.
// Self-contained so features can include it.

// volk must come before every Vulkan header, so Main/Resources.h goes first.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ViewEpoch.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowFrame.h"

#include <ORGModuleServices/BuildInputs.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	/**
	 * @brief The shadow view slots' buffers and shadow maps as the render thread last made them. A shadow build declares
	 * exactly these, never the live ShadowResources' vectors.
	 */
	struct ShadowSlotBuffers
	{
		std::vector<Versioned> drawArgs, bucketCounts;
		std::vector<std::uint32_t> drawArgsOwner;
		std::vector<std::shared_ptr<org::Buffer>> count;
		std::array<FrameAnchors::Texture, kShadowDepthTargets> depth{};
		std::array<std::uint32_t, kShadowDepthTargets> depthLayers{};

		/** @brief Whether this slot's arguments are declared under it, since a pass declares each buffer once. */
		bool OwnsDrawArgs(std::size_t a_slot) const { return a_slot >= drawArgsOwner.size() || drawArgsOwner[a_slot] == a_slot; }
	};

	/** @brief Render thread: a_shadow's slot buffers and shadow maps as they are now. */
	ShadowSlotBuffers MakeShadowSlotBuffers(const ShadowResources& a_shadow);
	/** @brief Render thread: whether a_slots holds exactly what a_shadow has now. */
	bool SameShadowSlotBuffers(const ShadowSlotBuffers& a_slots, const ShadowResources& a_shadow);

	/** @brief One view's part of a build: its resources and what its import set on them. */
	struct ViewBuild
	{
		std::shared_ptr<ViewResources> resources;
		std::shared_ptr<Resources> main;
		FrameAnchors::Texture target;
		std::shared_ptr<org::PixelBuffer> depth;
		std::uint32_t width = 0, height = 0;

		bool operator==(const ViewBuild&) const = default;
	};

	/** @brief What every DCLF epoch graph is built from. Immutable once made. */
	struct BuildState
	{
		// Moves with every new value. A snapshot is made only for the built value or an older one.
		std::uint64_t serial = 0;
		std::shared_ptr<const GraphConditions> conditions;
		std::shared_ptr<Resources> main;
		// The views drawing from the main resources when this was made.
		std::array<std::shared_ptr<ViewResources>, static_cast<std::size_t>(Api::EngineView::Count)> mainViews{};
		std::array<ViewBuild, kViews> views{};
		std::shared_ptr<ShadowResources> shadow;
		std::shared_ptr<const ShadowSlotBuffers> shadowSlots;
		std::vector<std::shared_ptr<const void>> passStates;

		/** @brief The view's part, empty for a view not made. */
		const ViewBuild& View(ViewKind a_kind) const { return views[static_cast<std::size_t>(a_kind)]; }
		/** @brief The part whose resources are a_view, null for none. */
		const ViewBuild* ViewOf(const ViewResources* a_view) const;
		/** @brief The conditions this state builds for. Defaults when it has none. */
		const GraphConditions& Conditions() const;
	};

	/**
	 * @brief Inside a graph build, the state it builds from: the value the build manager took for it on the render thread when it
	 * handed the build out (RenderGraph::Builds::Input). The extension holds it for the build, and its passes capture what they read
	 * from it. Null, with a warning logged with a_who, outside a build.
	 */
	std::shared_ptr<const BuildState> BuildingState(const char* a_who);

	/** @brief The build states' serials, the built one and the attached one (OMS BuildInputs). */
	using BuildStates = org::services::BuildInputs<BuildState>;
	/** @brief DCLF's one BuildStates. */
	BuildStates& BuildStateInputs();

	/**
	 * @brief Render thread: a_state is what the attached graphs are built from. Its conditions become the attached conditions.
	 * Called when a snapshot made for it is adopted, with the staged builds it was recorded on.
	 */
	void PublishAttachedBuildState(std::shared_ptr<const BuildState> a_state);
	/** @brief Any thread: the attached build state, null before the first. */
	std::shared_ptr<const BuildState> AttachedBuildState();
	/** @brief Any thread: the attached build state's serial, 0 before the first. */
	std::uint64_t AttachedBuildSerial();
	/** @brief Any thread: the built build state's serial, 0 before the first. */
	std::uint64_t BuiltBuildSerial();
}

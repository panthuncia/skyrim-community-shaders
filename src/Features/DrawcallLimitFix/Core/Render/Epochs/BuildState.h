#pragma once

// What DCLF's epoch graphs are built from (BuildState), as one immutable value.
//
// The render thread makes a new value when its live state changes and posts it with the revision inputs. Builds run on
// the scene worker while the render thread keeps changing, so a build reads only this value, never the live resources.
// A finished build attaches at a frame's end, and from then on its value is the attached one.
//
// Per-frame fields stay on the live resources, since only a pass's preparation or recording reads them.
// Self-contained so features can include it.

// volk must come before every Vulkan header, so Main/Resources.h goes first.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ViewEpoch.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowFrame.h"

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
		// Moves with every new value. A snapshot made from inputs of another serial than the attached one is stale.
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
	};

	/** @brief Sets the calling thread's build state for one graph build. Extensions made meanwhile read it, and so does CurrentGraphConditions. */
	class BuildScope
	{
	public:
		explicit BuildScope(std::shared_ptr<const BuildState> a_state);
		~BuildScope();
		BuildScope(const BuildScope&) = delete;
		BuildScope& operator=(const BuildScope&) = delete;

		/** @brief The state this scope builds from. */
		const std::shared_ptr<const BuildState>& State() const { return state; }

	private:
		std::shared_ptr<const BuildState> state;
		const BuildScope* previous = nullptr;
	};

	/** @brief The calling thread's build scope's state, null outside one. */
	const BuildState* ScopedBuildState();

	/**
	 * @brief Inside a graph build, the state it builds from. Outside a BuildScope this is an error logged with a_who,
	 * except on the render thread, where the host builds at an epoch's first submission.
	 */
	std::shared_ptr<const BuildState> BuildingState(const char* a_who);

	/** @brief Render thread, once at install: where BuildingState takes the render thread's newest value from. */
	void SetBuildStateSource(std::function<std::shared_ptr<const BuildState>()> a_source);

	/** @brief Render thread: a_state is what the attached graphs are built from. Its conditions become the attached conditions. */
	void PublishAttachedBuildState(std::shared_ptr<const BuildState> a_state);
	/** @brief Any thread: the attached build state, null before the first. */
	std::shared_ptr<const BuildState> AttachedBuildState();
	/** @brief Any thread: the attached build state's serial, 0 before the first. */
	std::uint64_t AttachedBuildSerial();

	/** @brief Whether DCLF's epochs build on the scene worker and attach at a frame's end (CS_DCLF_ASYNC_BUILDS, default on). */
	bool AsyncGraphBuilds();
}

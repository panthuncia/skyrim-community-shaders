#pragma once

// What Drawcall Limit Fix's epoch graphs are built from (BuildState), as one immutable value.
//
// The render thread makes it from its live state when that state changes (Impl::CurrentBuildState) and posts it with the revision
// inputs (RevisionInputs::build). A graph build reads only this value, never the live resources: the build runs on the scene worker
// (the "DCLF graph builds" lane) while the render thread keeps changing its state. The build that answers a change attaches at a frame's
// end (the build manager's external epochs), and from then on its value is the attached one (AttachedBuildState): the conditions the
// frame, the preparations and the revisions read are the attached graph's.
//
// Per-frame fields (latched targets, labels, frame buffers) stay on the live resources: only a pass's preparation or recording reads them.
// Self-contained (features include it); Internal.h includes it in its order.

// volk must precede every Vulkan header in this translation unit (Main/Resources.h first).
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
	 * @brief The shadow view slots' buffers and the shadow maps, as the render thread last made them (AddShadowViewSlots, the shadow map
	 * imports). The revision code counts and grows the slots by it (RevisionInputs::shadowSlots), and a shadow epoch's build declares exactly
	 * these (BuildState::shadowSlots), never the live ShadowResources' vectors.
	 */
	struct ShadowSlotBuffers
	{
		std::vector<Versioned> drawArgs, bucketCounts;
		std::vector<std::uint32_t> drawArgsOwner;
		std::vector<std::shared_ptr<org::Buffer>> count;
		std::array<FrameAnchors::Texture, kShadowDepthTargets> depth{};
		std::array<std::uint32_t, kShadowDepthTargets> depthLayers{};

		/** @brief Whether this slot's arguments are declared under it (a pass declares each buffer once). */
		bool OwnsDrawArgs(std::size_t a_slot) const { return a_slot >= drawArgsOwner.size() || drawArgsOwner[a_slot] == a_slot; }
	};

	/** @brief Render thread: a_shadow's slots' buffers and shadow maps, as they are now. */
	ShadowSlotBuffers MakeShadowSlotBuffers(const ShadowResources& a_shadow);
	/** @brief Render thread: whether a_slots holds exactly what a_shadow has now (MakeShadowSlotBuffers would make the same). */
	bool SameShadowSlotBuffers(const ShadowSlotBuffers& a_slots, const ShadowResources& a_shadow);

	/** @brief One view's part of a build: its resources and what its import set on them (the target, DCLF's depth, the size, the main). */
	struct ViewBuild
	{
		std::shared_ptr<ViewResources> resources;
		std::shared_ptr<Resources> main;
		FrameAnchors::Texture target;
		std::shared_ptr<org::PixelBuffer> depth;
		std::uint32_t width = 0, height = 0;

		bool operator==(const ViewBuild&) const = default;
	};

	/** @brief What every Drawcall Limit Fix epoch graph is built from. Immutable once made. */
	struct BuildState
	{
		// Moves with every new value (the structure serial). A snapshot made from inputs of another serial than the attached one is stale.
		std::uint64_t serial = 0;
		std::shared_ptr<const GraphConditions> conditions;
		std::shared_ptr<Resources> main;
		// The views drawing from the main resources (Resources::views), as they were when this was made.
		std::array<std::shared_ptr<ViewResources>, static_cast<std::size_t>(Api::EngineView::Count)> mainViews{};
		std::array<ViewBuild, kViews> views{};
		std::shared_ptr<ShadowResources> shadow;
		std::shared_ptr<const ShadowSlotBuffers> shadowSlots;
		// The passes' setup states (SceneBuffers::passStates of the main resources' scene).
		std::vector<std::shared_ptr<const void>> passStates;

		/** @brief The view's part; empty for a view not made. */
		const ViewBuild& View(ViewKind a_kind) const { return views[static_cast<std::size_t>(a_kind)]; }
		/** @brief The part of the view whose resources are a_view (null: none of this build's). */
		const ViewBuild* ViewOf(const ViewResources* a_view) const;
	};

	/**
	 * @brief Sets the calling thread's build state for one graph build (RAII). Every Drawcall Limit Fix extension made on this thread meanwhile
	 * reads it (BuildingState), and CurrentGraphConditions returns its conditions.
	 */
	class BuildScope
	{
	public:
		explicit BuildScope(std::shared_ptr<const BuildState> a_state);
		~BuildScope();
		BuildScope(const BuildScope&) = delete;
		BuildScope& operator=(const BuildScope&) = delete;

	private:
		std::shared_ptr<const BuildState> state;
		const BuildScope* previous = nullptr;
	};

	/** @brief The calling thread's build scope's state; null outside one. */
	const BuildState* ScopedBuildState();

	/**
	 * @brief Inside a graph build: the state it builds from. Outside a BuildScope it is an error, logged with a_who: on the render thread (a
	 * build the host makes at an epoch's first submission) the render thread's newest value is returned, elsewhere too, but logged as a defect.
	 */
	std::shared_ptr<const BuildState> BuildingState(const char* a_who);

	/** @brief Render thread, once at install: where BuildingState takes the render thread's newest value from (Impl::CurrentBuildState). */
	void SetBuildStateSource(std::function<std::shared_ptr<const BuildState>()> a_source);

	/**
	 * @brief Render thread: a_state is what the attached graphs are built from (every Drawcall Limit Fix build asked for is attached, or
	 * none was asked for). Its conditions become the attached conditions (CurrentGraphConditions outside a build, GraphBuiltForCpu).
	 */
	void PublishAttachedBuildState(std::shared_ptr<const BuildState> a_state);
	/** @brief Any thread: the attached build state; null before the first. */
	std::shared_ptr<const BuildState> AttachedBuildState();
	/** @brief Any thread: the attached build state's serial (0 before the first). */
	std::uint64_t AttachedBuildSerial();

	/** @brief Whether Drawcall Limit Fix's epochs build on the scene worker and attach at a frame's end (CS_DCLF_ASYNC_BUILDS, default on). */
	bool AsyncGraphBuilds();
}

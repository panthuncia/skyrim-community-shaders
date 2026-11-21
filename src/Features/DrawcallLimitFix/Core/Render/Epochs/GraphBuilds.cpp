// DCLF's epoch graph builds: the build state the render thread makes, and the built state settled after each build manager
// poll.
//
// A structural change goes like this. The render thread changes its state and asks for builds. The build manager hands
// them to its build lane with the newest build state. Frames keep the attached graphs meanwhile, and no snapshot is made
// for the new state until its builds are staged. Each finished build is staged at the next poll. Once none is pending,
// the newest state is the built one, and the snapshot builder records on the staged graphs. A Begin that adopts that
// snapshot attaches the staged graphs with it (plan step 10a). An epoch's first build goes the same way, and attaches when
// it is staged, since there is no graph to keep.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/BuildState.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "RenderGraph/BuildManager.h"

#include <span>
#include <utility>

namespace DCLF
{
	namespace
	{
		bool SameBuild(const Draws::BuildState& a_left, const Draws::BuildState& a_right)
		{
			return a_left.conditions == a_right.conditions && a_left.main == a_right.main && a_left.mainViews == a_right.mainViews &&
			       a_left.views == a_right.views && a_left.shadow == a_right.shadow && a_left.shadowSlots == a_right.shadowSlots &&
			       a_left.passStates == a_right.passStates;
		}

		/** @brief Whether any DCLF epoch has a graph. */
		bool AnyDclfGraph(org::GraphHost& a_host)
		{
			for (std::uint32_t segment = 0; segment < static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count); ++segment)
				if (DCLF::IsDclfSegment(static_cast<RenderGraphRuntime::Segment>(segment)) &&
					a_host.Graph(RenderGraphRuntime::EpochOf(static_cast<RenderGraphRuntime::Segment>(segment))))
					return true;
			return false;
		}

		/** @brief Whether a_epochs names one of DCLF's epochs. Epochs are segment values. */
		bool AnyDclfEpoch(std::span<const std::uint32_t> a_epochs)
		{
			for (const auto epoch : a_epochs)
				if (epoch < static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count) &&
					DCLF::IsDclfSegment(static_cast<RenderGraphRuntime::Segment>(epoch)))
					return true;
			return false;
		}

		// Render thread. Set while DCLF has had no graph since its builds began, so its first builds are made from the newest state.
		bool firstBuildsPending = false;
	}

	std::shared_ptr<const Draws::BuildState> IndirectDraws::Impl::CurrentBuildState()
	{
		// Everything a build reads that the render thread changes. Compared with the last value so the serial moves only on a change.
		Draws::BuildState next;
		next.conditions = Draws::RequestedGraphConditions();
		// A new display size's main resources once they are handed over. The frame takes them with a scene built on these graphs.
		const auto& main = AheadMain();
		next.main = main;
		if (main)
			for (std::size_t v = 0; v < next.mainViews.size(); ++v)
				next.mainViews[v] = main->views[v].lock();
		for (const auto& slot : kViewSlots) {
			const auto& view = View(slot.view).resources;
			if (!view)
				continue;
			auto& out = next.views[ViewIndex(slot.view)];
			out.resources = view;
			// A view keeps the frame's main resources until the frame adopts the new ones (AdoptPendingMain).
			out.main = pendingResources && view->main == resources ? pendingResources : view->main;
			out.target = view->target;
			out.depth = view->depth;
			out.width = view->width;
			out.height = view->height;
		}
		next.shadow = shadowEngine->shadow;
		next.shadowSlots = shadowEngine->SlotBuffers();
		if (const auto& sceneBuffers = main && main->scene ? main->scene : scene)
			next.passStates = sceneBuffers->passStates;
		if (buildState && SameBuild(*buildState, next))
			return buildState;
		next.serial = (buildState ? buildState->serial : 0) + 1;
		buildState = std::make_shared<const Draws::BuildState>(std::move(next));
		return buildState;
	}

	void IndirectDraws::Impl::SettleBuildState()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !RenderGraphRuntime::Get().IsActive())
			return;
		const auto state = CurrentBuildState();
		if (!AnyDclfGraph(*host))
			firstBuildsPending = true;
		if (state->serial == Draws::BuiltBuildSerial())
			return;
		// While a build is pending, the first ones too, the staged or attached graphs are the old ones.
		if (!RenderGraphRuntime::Get().BuildsSettled())
			return;
		Draws::PublishBuiltBuildState(state->serial);
		// The first builds attach when they are staged, so their state is attached too.
		if (std::exchange(firstBuildsPending, false))
			Draws::PublishAttachedBuildState(state);
		// Post the inputs again even when nothing else moved, since the builder holds its make until this serial is built.
		PostRevisionInputs();
		(void)SnapshotPump();
		snapshotInputsMoved.store(true, std::memory_order_release);
		WakeSnapshotBuilder();
	}

	void IndirectDraws::Impl::RegisterGraphBuilds()
	{
		auto& runtime = RenderGraphRuntime::Get();
		auto* builds = runtime.IsActive() ? runtime.Builds() : nullptr;
		if (graphBuildsRegistered || !builds)
			return;
		graphBuildsRegistered = true;
		// Every build carries the newest value from its hand-out on the render thread, and its extensions make their passes from it.
		// The host's own builds on the render thread take it when they run.
		builds->SetBuildInput([this]() -> std::shared_ptr<const void> { return CurrentBuildState(); });
		// After every poll, since any may stage DCLF's epochs.
		builds->SetPollListener([this](std::span<const std::uint32_t> a_changed) {
			SettleBuildState();
			// New staged generations, which the snapshot builder records on.
			if (AnyDclfEpoch(a_changed))
				PostRevisionInputs();
		});
		logger::info("[DCLF] graph builds: on the build manager's lane from the build state; staged, and attached with a snapshot at a Begin");
	}
}

// DCLF's epoch graph builds: the build state the render thread makes, the scene worker's builds of it, and the built
// state settled at each build manager poll.
//
// A structural change goes like this. The render thread changes its state and asks for builds. The build manager hands
// them to the "DCLF graph builds" lane with the newest build state. Frames keep the attached graphs meanwhile, and no
// snapshot is made for the new state until its builds are staged. Each finished build is staged at the next poll. Once
// none is pending, the newest state is the built one, and the snapshot builder records on the staged graphs. A Begin that
// adopts that snapshot attaches the staged graphs with it (plan step 10a).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/BuildState.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "RenderGraph/BuildManager.h"

#include <deque>
#include <mutex>

namespace DCLF
{
	namespace
	{
		/** @brief The builds the build manager posted, run one at a time on the scene worker's preparation pool. */
		struct BuildLane
		{
			std::mutex mutex;
			std::deque<std::function<void()>> queue;  // mutex
		};

		BuildLane& Lane()
		{
			// Never destroyed, since a build may still be queued while the process tears down.
			static auto* lane = new BuildLane;
			return *lane;
		}

		void DrainLane()
		{
			auto& lane = Lane();
			for (;;) {
				std::function<void()> build;
				{
					std::scoped_lock lock(lane.mutex);
					if (lane.queue.empty())
						return;
					build = std::move(lane.queue.front());
					lane.queue.pop_front();
				}
				DCLF_ZONE("CS.DCLF.GraphBuild");
				build();
			}
		}

		org::async::SerializedTaskPump& LanePump()
		{
			static auto* pump = SceneScheduler::MakeLanePump(
				SceneScheduler::Executor(), SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "DCLF graph builds",
				[] { DrainLane(); },
				[] { logger::error("[DCLF] the graph build lane was refused by DCLF's executor; its queued builds wait for the next wake"); });
			return *pump;
		}

		bool SameBuild(const Draws::BuildState& a_left, const Draws::BuildState& a_right)
		{
			return a_left.conditions == a_right.conditions && a_left.main == a_right.main && a_left.mainViews == a_right.mainViews &&
			       a_left.views == a_right.views && a_left.shadow == a_right.shadow && a_left.shadowSlots == a_right.shadowSlots &&
			       a_left.passStates == a_right.passStates;
		}

		/** @brief Whether any DCLF epoch has a graph. Before that, the host builds each at its first submission. */
		bool AnyDclfGraph(org::GraphHost& a_host)
		{
			for (std::uint32_t segment = 0; segment < static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count); ++segment)
				if (RenderGraphRuntime::IsDclfSegment(static_cast<RenderGraphRuntime::Segment>(segment)) &&
					a_host.Graph(RenderGraphRuntime::EpochOf(static_cast<RenderGraphRuntime::Segment>(segment))))
					return true;
			return false;
		}
	}

	std::shared_ptr<const Draws::BuildState> IndirectDraws::Impl::CurrentBuildState()
	{
		// Everything a build reads that the render thread changes. Compared with the last value so the serial moves only on a change.
		Draws::BuildState next;
		next.conditions = Draws::RequestedGraphConditions();
		next.main = resources;
		if (resources)
			for (std::size_t v = 0; v < next.mainViews.size(); ++v)
				next.mainViews[v] = resources->views[v].lock();
		for (const auto& slot : kViewSlots) {
			const auto& view = View(slot.view).resources;
			if (!view)
				continue;
			auto& out = next.views[ViewIndex(slot.view)];
			out.resources = view;
			out.main = view->main;
			out.target = view->target;
			out.depth = view->depth;
			out.width = view->width;
			out.height = view->height;
		}
		next.shadow = shadowEngine->shadow;
		next.shadowSlots = shadowEngine->SlotBuffers();
		if (const auto& sceneBuffers = resources && resources->scene ? resources->scene : scene)
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
		if (state->serial == Draws::BuiltBuildSerial())
			return;
		// While a build is pending, the staged or attached graphs are the old ones.
		const bool graphs = AnyDclfGraph(*host);
		if (graphs && !RenderGraphRuntime::Get().DclfBuildsSettled())
			return;
		Draws::PublishBuiltBuildState(state->serial);
		// Before any DCLF graph exists, the host builds each at its first submission from the newest state, so it is attached too.
		if (!graphs)
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
		// A build without a scope on the render thread, the host's first build of an epoch, takes the newest value.
		Draws::SetBuildStateSource([this] { return CurrentBuildState(); });
		RenderGraph::BuildManager::ExternalBuilder builder;
		// Every build the manager makes on the render thread reads the newest value, held for the build.
		builder.ownerScope = [this]() -> std::shared_ptr<void> { return std::make_shared<Draws::BuildScope>(CurrentBuildState()); };
		if (Draws::AsyncGraphBuilds()) {
			// On the scene worker, the build reads only the state made at the poll that handed it out.
			builder.post = [this](std::function<void()> a_build) {
				auto state = CurrentBuildState();
				{
					auto& lane = Lane();
					std::scoped_lock lock(lane.mutex);
					lane.queue.push_back([state = std::move(state), build = std::move(a_build)] {
						Draws::BuildScope scope(state);
						build();
					});
				}
				(void)LanePump().Notify();
				return true;
			};
		}
		builder.boundary = [this](bool a_staged) {
			SettleBuildState();
			// New staged generations, which the snapshot builder records on.
			if (a_staged)
				PostRevisionInputs();
		};
		builds->SetExternalBuilder(std::move(builder));
		logger::info("[DCLF] graph builds: {}; staged, and attached with a snapshot at a Begin",
			Draws::AsyncGraphBuilds() ? "on the scene worker from the build state" : "on the render thread at the build point (CS_DCLF_ASYNC_BUILDS=0)");
	}
}

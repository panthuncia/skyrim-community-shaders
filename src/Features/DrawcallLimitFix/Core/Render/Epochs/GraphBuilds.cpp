// Drawcall Limit Fix's epoch graph builds: the build state the render thread makes (CurrentBuildState), the scene worker's builds of it
// (RegisterGraphBuilds: the build manager's external builder), and the attached state settled at frame boundaries (SettleBuildState).
//
// The order of a structural change:
// 1. The render thread changes its state (new resources, an import, more shadow slots, new conditions) and asks for the epochs' builds.
// 2. At the next poll the build manager hands the builds to the "DCLF graph builds" lane, each with the newest build state.
// 3. Meanwhile frames keep the attached graphs and the snapshot made for them: a snapshot of a newer build state is stale, and the
//    snapshot builder makes none until the builds attach (MakeRevision).
// 4. At a frame's end, after the frame's epochs were submitted, the finished builds attach together (AttachEpochGraph). Once none is
//    asked for or in flight, the newest build state becomes the attached one, and the inputs are posted for the new graphs.
// 5. The host's thread records the new graphs while the engine updates the next frame; that frame's start adopts the new snapshot (a
//    wait there, if any, is logged).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/BuildState.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
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
			// Never destroyed: a build may still be queued while the process tears down (the build manager's own check drops it).
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

		/** @brief Whether any of Drawcall Limit Fix's epochs has a graph (before that, the host builds each at its first submission). */
		bool AnyDclfGraph(const org::GraphHost& a_host)
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
		// Everything a build reads that the render thread changes, as it is now. Compared with the last value, so the serial moves only
		// with a change.
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
		if (state->serial == Draws::AttachedBuildSerial())
			return;
		// A build still asked for or running: the attached graphs are the old ones until it attaches. With no graph yet, the host's first
		// builds (at each epoch's first submission) are of this value.
		if (AnyDclfGraph(*host) && !RenderGraphRuntime::Get().DclfBuildsSettled())
			return;
		Draws::PublishAttachedBuildState(state);
		// The inputs carry the attached conditions (their CPU pipeline and forward family): posted again, and the builder is woken even
		// when nothing else moved, since it holds its make until this serial is attached.
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
		// A build without a scope on the render thread (the host's first build of an epoch) takes the render thread's newest value.
		Draws::SetBuildStateSource([this] { return CurrentBuildState(); });
		RenderGraph::BuildManager::ExternalBuilder builder;
		// Every build the manager makes on the render thread (CS_DCLF_ASYNC_BUILDS=0, a main resources change under claims, a shader
		// reload, setup) reads the render thread's newest value, held for the build.
		builder.ownerScope = [this]() -> std::shared_ptr<void> { return std::make_shared<Draws::BuildScope>(CurrentBuildState()); };
		if (Draws::AsyncGraphBuilds()) {
			// On the scene worker, from the build state made at the poll that handed the build out (render thread). The build reads only
			// that value: the render thread may change its live state meanwhile.
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
		builder.boundary = [this](bool a_attached) {
			if (a_attached)
				graphsAttachedFrame = Scene::FrameView::Get().GetFrame();
			SettleBuildState();
		};
		builds->SetExternalBuilder(std::move(builder));
		logger::info("[DCLF] graph builds: {}", Draws::AsyncGraphBuilds() ? "on the scene worker from the build state, attached at a frame's end" :
																			"on the render thread at the build point (CS_DCLF_ASYNC_BUILDS=0)");
	}
}

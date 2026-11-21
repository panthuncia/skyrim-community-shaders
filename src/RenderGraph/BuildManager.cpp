#include "RenderGraph/BuildManager.h"

#include "RenderGraph/Async/Lanes.h"
#include "RenderGraph/CensusWindow.h"
#include "RenderGraph/Frame/BoundaryCheck.h"
#include "RenderGraph/Frame/Recipe.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/RenderGraph/RenderGraph.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace RenderGraph
{
	namespace
	{
		/** @brief Tells the frame boundary census of the build manager's polls, attaches and refusals. */
		class BoundaryObserver final : public org::services::BuildObserver
		{
		public:
			void InPoll(std::string_view a_point, void (*a_body)(void*), void* a_context) override
			{
				const Frame::Boundary::Caller caller(a_point);
				a_body(a_context);
			}

			void Attached(std::string_view a_epoch) override { Frame::Boundary::NoteAttach(a_epoch); }

			void Refused(org::services::BuildRefusalSite a_site, std::string_view a_reason) override
			{
				Frame::Boundary::NoteRefusal(SiteName(a_site), a_reason);
			}

		private:
			static std::string_view SiteName(org::services::BuildRefusalSite a_site)
			{
				switch (a_site) {
				case org::services::BuildRefusalSite::Staging:
					return "staging";
				case org::services::BuildRefusalSite::Attach:
					return "attach";
				case org::services::BuildRefusalSite::StagedSet:
					break;
				}
				return "revision";
			}
		};

		BoundaryObserver& Observer()
		{
			static BoundaryObserver observer;
			return observer;
		}
	}

	std::unique_ptr<BuildManager> MakeBuildManager(BuildManager::Desc a_desc)
	{
		// One build at a time, in request order, on the shared executor's preparation pool.
		a_desc.executor = &Frame::SharedExecutor();
		a_desc.lane = Frame::FatalOnRefusal({ "CS graph builds", Frame::Executor::Preparation, org::async::TaskDispatch::Cpu });
		// The recipe check reads the schedule on the build thread and takes it at the attach.
		auto* host = a_desc.host;
		a_desc.afterBuild = [host](std::uint32_t a_epoch, org::RenderGraph& a_graph) {
			if (!Frame::Recipes::Traced(a_epoch))
				return;
			if (auto trace = Frame::Recipes::TraceEpochGraph(a_graph, a_epoch)) {
				Frame::Recipes::Accept(a_epoch, trace);
				(void)org::services::DeferToAttach([host, a_epoch, trace = std::move(trace)]() mutable {
					Frame::Recipes::NoteTrace(a_epoch, host->BuildGeneration(a_epoch), std::move(trace));
				});
			}
		};
		a_desc.isSteadyState = [] { return CensusWindow::Current() == CensusWindow::Kind::World; };
		a_desc.steadyStateName = "first world frame";
		a_desc.observer = &Observer();
		a_desc.log = [](org::services::BuildLogLevel a_level, std::string_view a_line) {
			switch (a_level) {
			case org::services::BuildLogLevel::Info:
				logger::info("{}", a_line);
				break;
			case org::services::BuildLogLevel::Warning:
				logger::warn("{}", a_line);
				break;
			case org::services::BuildLogLevel::Error:
				logger::error("{}", a_line);
				break;
			}
		};
		return std::make_unique<BuildManager>(std::move(a_desc));
	}

	namespace Builds
	{
		namespace
		{
			BuildManager* Manager()
			{
				auto& runtime = RenderGraphRuntime::Get();
				return runtime.IsActive() ? runtime.Builds() : nullptr;
			}
		}

		namespace
		{
			/** @brief A request Ask keeps for the next adoption point. */
			struct AskedBuild
			{
				Wanted wanted;
				std::string why;
			};

			struct AskQueue
			{
				std::mutex mutex;
				std::vector<AskedBuild> kept;
				bool framesBegun = false;
			};

			AskQueue& Asked()
			{
				static AskQueue queue;
				return queue;
			}

			std::atomic<std::uint64_t> structuralBuilds{ 0 };

			/** @brief Makes a request: before the first frame, or inside the adoption point. */
			void IssueAsked(Wanted a_wanted, std::string_view a_why)
			{
				auto* manager = Manager();
				if (!manager) {
					// Nothing is built while the graph is inactive, so a change applies at once.
					if (a_wanted.change)
						a_wanted.change();
					return;
				}
				std::string reason(a_why);
				if (a_wanted.structural) {
					const auto count = structuralBuilds.fetch_add(1, std::memory_order_relaxed) + 1;
					const bool every = a_wanted.all || static_cast<bool>(a_wanted.change);
					logger::info("[ORG] structural rebuild {} ({}): {}", count,
						every ? std::string("every epoch") : fmt::format("{} epochs, {} ids", a_wanted.epochs.size(), a_wanted.ids.size()), a_why);
					reason = fmt::format("structural: {}", a_why);
				}
				// A change rebuilds every epoch on its own.
				if (a_wanted.change) {
					manager->Change(std::move(a_wanted.change), reason);
					return;
				}
				if (a_wanted.all) {
					manager->RequestAll(reason);
					return;
				}
				for (const auto epoch : a_wanted.epochs)
					manager->Request(epoch, reason);
				for (const auto& id : a_wanted.ids)
					manager->RequestFor(id, reason);
			}
		}

		void Ask(Wanted a_wanted, std::string_view a_why, const std::source_location& a_where)
		{
			// A structural request means an identity no version covers changed. Inside the frame on the render thread that is a late
			// change, counted here once (a registration's own Change covers the requests it makes).
			std::optional<Frame::Boundary::Change> late;
			if (a_wanted.structural)
				late.emplace(a_why, "structural rebuild", a_where);
			{
				auto& queue = Asked();
				const std::lock_guard lock(queue.mutex);
				if (queue.framesBegun && !Frame::Boundary::InAdoptionPoint()) {
					queue.kept.push_back({ std::move(a_wanted), std::string(a_why) });
					return;
				}
			}
			IssueAsked(std::move(a_wanted), a_why);
		}

		void IssueAdoptionRequests()
		{
			std::vector<AskedBuild> kept;
			{
				auto& queue = Asked();
				const std::lock_guard lock(queue.mutex);
				queue.framesBegun = true;
				kept = std::exchange(queue.kept, {});
			}
			for (auto& asked : kept)
				IssueAsked(std::move(asked.wanted), asked.why);
		}

		bool Defer(std::function<void()> a_attached, std::function<void(const std::string&)> a_failed)
		{
			return org::services::DeferToAttach(std::move(a_attached), std::move(a_failed));
		}

		bool DeferStaged(std::function<void()> a_staged)
		{
			return org::services::DeferToStaging(std::move(a_staged));
		}

		std::shared_ptr<const void> Input()
		{
			if (const auto* build = org::services::CurrentBuild())
				return build->input;
			// The host's own builds on the owner thread (an epoch's first build, setup) read the owner's state now.
			auto* manager = Manager();
			return manager ? manager->OwnerInput() : nullptr;
		}
	}
}


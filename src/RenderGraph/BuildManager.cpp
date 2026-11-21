#include "RenderGraph/BuildManager.h"

#include "RenderGraph/Async/Lanes.h"
#include "RenderGraph/CensusWindow.h"
#include "RenderGraph/Frame/BoundaryCheck.h"
#include "RenderGraph/Frame/Recipe.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <Resources/ResourceIdentifier.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RenderGraph
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		double MillisecondsSince(Clock::time_point a_start)
		{
			return std::chrono::duration<double, std::milli>(Clock::now() - a_start).count();
		}

		/** @brief What a build's extensions deferred to its attach or its failure. */
		struct BuildContext
		{
			std::vector<std::function<void()>> attached;
			std::vector<std::function<void(const std::string&)>> failed;
			std::vector<std::function<void()>> staged;  // run when the graph is staged
			std::shared_ptr<const void> input;  // what the build is made from (SetBuildInput), taken when it was handed out
		};

		thread_local BuildContext* t_context = nullptr;

		class ContextScope
		{
		public:
			explicit ContextScope(BuildContext& a_context) :
				previous(std::exchange(t_context, &a_context)) {}
			~ContextScope() { t_context = previous; }
			ContextScope(const ContextScope&) = delete;
			ContextScope& operator=(const ContextScope&) = delete;

		private:
			BuildContext* previous;
		};

		/** @brief A request made off the owner thread, applied at the next poll. */
		struct PendingRequest
		{
			enum class Kind
			{
				Epoch,
				All,
				For,
				Change
			};
			Kind kind = Kind::All;
			std::uint32_t epoch = 0;
			std::string id, reason;
			std::function<void()> change;
		};

		struct Window
		{
			std::uint64_t builds = 0, synchronous = 0, attached = 0, superseded = 0, rejected = 0, failed = 0, stale = 0;
			double buildMs = 0.0, buildMaxMs = 0.0, attachMs = 0.0, attachMaxMs = 0.0;

			bool Empty() const { return !builds && !synchronous && !attached && !superseded && !rejected && !failed && !stale; }
		};

		constexpr std::uint32_t kMaxSupersededInARow = 3;  // then the build is attached anyway, and built again after
		constexpr std::uint32_t kMaxRejectedInARow = 3;     // then the epoch counts as failed until it is asked for again
		constexpr std::uint64_t kStatsFrames = 3000;
	}

	struct BuildManager::Impl
	{
		struct Job
		{
			std::uint32_t epoch = 0;
			std::uint64_t group = 0;
			std::uint64_t requests = 0;    // the epoch's request count when the job was handed out
			std::uint64_t generation = 0;  // the epoch's build generation then
			std::uint64_t frame = 0;
			std::string reason;
			// Taken on the owner thread when the job is made, so a build on any thread reads the state of its hand-out.
			std::shared_ptr<const void> input;
		};

		struct Done
		{
			Job job;
			std::unique_ptr<org::GraphHost::EpochGraph> graph;
			BuildContext context;
			std::string error;  // a throw out of BuildEpochGraph itself
		};

		struct Group
		{
			std::size_t remaining = 0;
			std::vector<std::unique_ptr<Done>> done;
		};

		Desc desc;
		std::unordered_set<std::uint32_t> staged;
		std::unordered_set<std::uint32_t> laneOnly;
		// Owner thread. Run by AttachStaged.
		std::unordered_map<std::uint32_t, std::vector<std::function<void()>>> stagedAttach;
		std::function<void(std::span<const std::uint32_t>)> pollListener;  // owner thread
		std::function<std::shared_ptr<const void>()> input;  // owner thread

		// The build lane's results.
		mutable std::mutex mutex;  // finished, stop
		std::vector<std::unique_ptr<Done>> finished;
		bool stop = false;
		// Held around every build on either thread. Extensions' build-time state assumes one build at a time.
		std::mutex buildMutex;

		// Owner thread.
		std::atomic<std::thread::id> owner{ std::this_thread::get_id() };  // read by Submit on any thread
		std::map<std::uint64_t, Group> groups;
		std::unordered_set<std::uint32_t> inFlight;
		std::unordered_map<std::uint32_t, std::uint64_t> failedAt;  // the request count its last build failed at
		std::unordered_map<std::uint32_t, std::uint32_t> supersededInARow;
		std::unordered_map<std::uint32_t, std::uint32_t> rejectedInARow;
		std::uint64_t groupSerial = 0;
		std::uint64_t frames = 0;
		bool worldStarted = false;
		std::uint64_t warnings = 0;
		std::uint64_t attachLogs = 0;
		Window window;

		std::mutex pendingMutex;
		std::vector<PendingRequest> pending;

		// The build lane: one build at a time, in request order, on the shared executor's preparation pool. Last, so it stops first.
		Frame::JobLane<Job> lane;

		explicit Impl(Desc a_desc) :
			desc(std::move(a_desc)), staged(desc.staged.begin(), desc.staged.end()), laneOnly(desc.laneOnly.begin(), desc.laneOnly.end()), lane({ "CS graph builds", Frame::Executor::Preparation, org::async::TaskDispatch::Cpu }, [this](Job& a_job) { RunJob(std::move(a_job)); })
		{
		}

		std::string Label(std::uint32_t a_epoch) const
		{
			return desc.label ? desc.label(a_epoch) : fmt::format("epoch {}", a_epoch);
		}

		bool OnOwner() const { return std::this_thread::get_id() == owner.load(std::memory_order_acquire); }

		/** @brief One queued build, on the build lane. */
		void RunJob(Job a_job)
		{
			auto done = Build(std::move(a_job));
			std::scoped_lock lock(mutex);
			finished.push_back(std::move(done));
		}

		std::unique_ptr<Done> Build(Job a_job)
		{
			auto done = std::make_unique<Done>();
			done->job = std::move(a_job);
			done->context.input = done->job.input;
			std::scoped_lock lock(buildMutex);
			ContextScope scope(done->context);
			try {
				done->graph = desc.host->BuildEpochGraph(done->job.epoch, done->job.reason);
				// The recipe check reads the schedule on the build thread and takes it at the attach.
				if (done->graph && done->graph->Built() && Frame::Recipes::Traced(done->job.epoch))
					if (auto trace = Frame::Recipes::TraceEpochGraph(*done->graph->Graph(), done->job.epoch)) {
						Frame::Recipes::Accept(done->job.epoch, trace);
						auto* host = desc.host;
						const auto epoch = done->job.epoch;
						done->context.attached.push_back([host, epoch, trace = std::move(trace)]() mutable {
							Frame::Recipes::NoteTrace(epoch, host->BuildGeneration(epoch), std::move(trace));
						});
					}
			} catch (const std::exception& e) {
				done->error = e.what();
			} catch (...) {
				done->error = "unknown exception";
			}
			return done;
		}

		/** @brief Owner thread: the input a build made now reads. */
		std::shared_ptr<const void> TakeInput() const
		{
			return input ? input() : nullptr;
		}

		/** @brief Tells the boundary check of an attach. */
		void NoteAttached(std::uint32_t a_epoch) const
		{
			Frame::Boundary::NoteAttach(Label(a_epoch));
		}

		void Apply(const PendingRequest& a_request)
		{
			auto& host = *desc.host;
			switch (a_request.kind) {
			case PendingRequest::Kind::Epoch:
				host.RequestRebuild(a_request.epoch, a_request.reason);
				break;
			case PendingRequest::Kind::All:
				host.RequestRebuild(a_request.reason);
				break;
			case PendingRequest::Kind::For:
				(void)host.RequestRebuildFor(org::ResourceIdentifier(a_request.id), a_request.reason);
				break;
			case PendingRequest::Kind::Change:
				// No build may capture what the change edits while it runs. A build handed out before is dropped at its attach and built again.
				if (a_request.change) {
					std::scoped_lock lock(buildMutex);
					a_request.change();
				}
				host.RequestRebuild(a_request.reason);
				break;
			}
		}

		void Submit(PendingRequest a_request)
		{
			// A change always waits for the poll, since here it could meet a build this thread holds.
			if (OnOwner() && a_request.kind != PendingRequest::Kind::Change) {
				Apply(a_request);
				return;
			}
			std::scoped_lock lock(pendingMutex);
			pending.push_back(std::move(a_request));
		}

		void ApplyPending()
		{
			std::vector<PendingRequest> requests;
			{
				std::scoped_lock lock(pendingMutex);
				requests.swap(pending);
			}
			for (const auto& request : requests)
				Apply(request);
		}

		/**
		* @brief The epoch is asked for and has a graph to keep running, or is lane-only and has no graph yet. It is not being built
		* or known to fail.
		*/
		bool Due(std::uint32_t a_epoch) const
		{
			auto& host = *desc.host;
			if (inFlight.contains(a_epoch))
				return false;
			if (!host.Graph(a_epoch)) {
				// Other epochs with no graph yet are built by the host at their first submission. A lane-only one is built here once
				// there is a build input, and its build attaches when it is staged. A failed one waits to be asked for again.
				if (!laneOnly.contains(a_epoch) || !input || host.EpochFailed(a_epoch))
					return false;
			} else if (!host.RebuildRequested(a_epoch)) {
				return false;
			}
			const auto failed = failedAt.find(a_epoch);
			return failed == failedAt.end() || failed->second != host.RebuildRequests(a_epoch);
		}

		Job MakeJob(std::uint32_t a_epoch, std::uint64_t a_group) const
		{
			auto& host = *desc.host;
			Job job;
			job.epoch = a_epoch;
			job.group = a_group;
			job.requests = host.RebuildRequests(a_epoch);
			job.generation = host.BuildGeneration(a_epoch);
			job.frame = frames;
			job.reason = host.RebuildReasons(a_epoch);
			if (job.reason.empty())
				job.reason = "no request named";
			job.input = TakeInput();
			return job;
		}

		void Collect()
		{
			std::vector<std::unique_ptr<Done>> taken;
			{
				std::scoped_lock lock(mutex);
				taken.swap(finished);
			}
			for (auto& done : taken) {
				auto& group = groups[done->job.group];
				if (group.remaining)
					--group.remaining;
				group.done.push_back(std::move(done));
			}
		}

		/** @brief Logs a failed build once per request count and keeps the old graph. a_notify is false for repeated host rejections. */
		void Fail(Done& a_done, const std::string& a_error, bool a_notify = true)
		{
			const auto epoch = a_done.job.epoch;
			auto& host = *desc.host;
			++window.failed;
			const bool logged = failedAt.contains(epoch) && failedAt[epoch] == a_done.job.requests;
			failedAt[epoch] = a_done.job.requests;
			const bool kept = host.Graph(epoch) != nullptr;
			if (!logged)
				logger::error("[ORG] build manager: {} failed to build ({}); not built again until it is asked for again. Why it was built: {}. "
							  "Error: {}",
					Label(epoch), kept ? "its previous graph keeps running" : "it has no graph", a_done.job.reason, a_error);
			if (!a_notify)
				return;
			if (!a_done.context.failed.empty()) {
				for (auto& failed : a_done.context.failed)
					if (failed)
						failed(a_error);
			} else if (desc.onFailed) {
				desc.onFailed(epoch, kept, a_error);
			}
		}

		/** @brief Attaches one finished build or drops it. Returns whether it was attached. */
		bool Finish(Done& a_done)
		{
			auto& host = *desc.host;
			const auto epoch = a_done.job.epoch;
			if (!a_done.graph || !a_done.graph->Built()) {
				Fail(a_done, a_done.graph ? a_done.graph->Error() : a_done.error);
				return false;
			}
			// Read now, since an attach takes the graph.
			const double buildMs = a_done.graph->BuildMilliseconds();
			++window.builds;
			window.buildMs += buildMs;
			window.buildMaxMs = (std::max)(window.buildMaxMs, buildMs);
			// Requested again since it was handed out, so drop it and rebuild.
			// After too many in a row it goes in anyway, since it is newer than the running graph.
			if (host.RebuildRequests(epoch) != a_done.job.requests && supersededInARow[epoch] < kMaxSupersededInARow) {
				++supersededInARow[epoch];
				++window.superseded;
				return false;
			}
			const auto begin = Clock::now();
			// A staged epoch is recorded by the host's thread first, and a frame revision (or a fragment adoption) attaches it.
			// The host attaches an epoch's first build at once, since it has no graph to keep.
			if (staged.contains(epoch)) {
				const auto stagedResult = host.StageEpochGraph(a_done.graph);
				if (stagedResult.staged) {
					supersededInARow.erase(epoch);
					rejectedInARow.erase(epoch);
					failedAt.erase(epoch);
					for (auto& fn : a_done.context.staged)
						if (fn)
							fn();
					stagedAttach[epoch] = std::move(a_done.context.attached);
					logger::info("[ORG] build manager: {} staged as build {}: built in {:.2f} ms off the render thread, staged in {:.3f} ms, {} frames after "
								 "it was asked for; it attaches with a frame revision; why: {}",
						Label(epoch), stagedResult.generation, buildMs, MillisecondsSince(begin),
						frames - a_done.job.frame, a_done.job.reason);
					return true;
				}
				if (!stagedResult.attached) {
					++window.rejected;
					RenderGraph::Frame::Boundary::NoteRefusal("staging", org::GraphHost::AttachRefusalName(stagedResult.refusal));
					logger::info("[ORG] build manager: {}'s build rejected at its staging ({}); built again", Label(epoch), stagedResult.reason);
					host.RequestRebuild(epoch, "its last build was rejected at its staging");
					return false;
				}
			}
			const auto result = a_done.graph ? host.AttachEpochGraph(a_done.graph) : org::GraphHost::AttachResult{ true, {} };
			const double attachMs = MillisecondsSince(begin);
			if (!result.attached) {
				++window.rejected;
				RenderGraph::Frame::Boundary::NoteRefusal("attach", org::GraphHost::AttachRefusalName(result.refusal));
				// If no other build answered meanwhile, ask again so the next poll builds against the host as it is now.
				const bool replaced = host.BuildGeneration(epoch) != a_done.job.generation;
				logger::info("[ORG] build manager: {}'s build rejected at the attach ({}){}", Label(epoch), result.reason,
					replaced ? "; another build replaced the graph meanwhile" : "; built again");
				if (replaced) {
					rejectedInARow.erase(epoch);
				} else if (++rejectedInARow[epoch] > kMaxRejectedInARow) {
					rejectedInARow.erase(epoch);
					Fail(a_done, "rejected at the attach " + std::to_string(kMaxRejectedInARow + 1) + " times in a row: " + result.reason, false);
				} else {
					host.RequestRebuild(epoch, "its last build was rejected at the attach");
				}
				return false;
			}
			supersededInARow.erase(epoch);
			rejectedInARow.erase(epoch);
			failedAt.erase(epoch);
			if (a_done.graph)
				NoteAttached(epoch);
			++window.attached;
			window.attachMs += attachMs;
			window.attachMaxMs = (std::max)(window.attachMaxMs, attachMs);
			for (auto& attached : a_done.context.attached)
				if (attached)
					attached();
			if (++attachLogs <= 200 || attachLogs % 100 == 0)
				logger::info("[ORG] build manager: {} attached{}: built in {:.2f} ms off the render thread, attached in {:.3f} ms on the render thread, "
							 "{} frames after it was asked for; why: {}",
					Label(epoch), attachLogs > 200 ? fmt::format(" (attach {})", attachLogs) : std::string(), buildMs, attachMs, frames - a_done.job.frame,
					a_done.job.reason);
			return true;
		}

		/** @brief Attaches or stages every group whose builds have all finished. a_changed gets each epoch staged or attached. */
		bool AttachFinished(std::vector<std::uint32_t>& a_changed)
		{
			Collect();
			bool attached = false;
			for (auto it = groups.begin(); it != groups.end();) {
				if (it->second.remaining) {
					++it;
					continue;
				}
				for (auto& done : it->second.done) {
					inFlight.erase(done->job.epoch);
					const bool one = Finish(*done);
					attached = one || attached;
					if (one)
						a_changed.push_back(done->job.epoch);
				}
				it = groups.erase(it);
			}
			return attached;
		}

		/** @brief Asks again for epochs whose alias placements are in a pool generation another build replaced. */
		void RequestStale()
		{
			auto& host = *desc.host;
			for (const auto epoch : host.StaleAliasPlacementEpochs()) {
				if (host.RebuildRequested(epoch) || inFlight.contains(epoch))
					continue;
				host.RequestRebuild(epoch, "its alias placements are in a pool another epoch's build grew");
				++window.stale;
			}
		}

		/** @brief Logs a render-thread build after the first world frame, rate-limited. */
		void WarnSynchronous(std::string_view a_what, std::string_view a_caller)
		{
			if (!worldStarted)
				return;
			if (++warnings <= 50 || warnings % 100 == 0)
				logger::warn("[ORG] build manager: graph built on the render thread after the first world frame ({}): {}; caller: {}", warnings, a_what,
					a_caller);
		}

		bool Settled() const
		{
			auto& host = *desc.host;
			for (std::size_t i = 0; i < host.GraphCount(); ++i) {
				const auto epoch = host.EpochAt(i);
				if (!laneOnly.contains(epoch))
					continue;
				if (inFlight.contains(epoch))
					return false;
				// A lane-only epoch with no graph waits for its first build, once there is a build input, unless that failed.
				if (!host.Graph(epoch)) {
					if (!input || host.EpochFailed(epoch))
						continue;
				} else if (!host.RebuildRequested(epoch)) {
					continue;
				}
				const auto failed = failedAt.find(epoch);
				if (failed == failedAt.end() || failed->second != host.RebuildRequests(epoch))
					return false;
			}
			return true;
		}

		/** @brief Hands the due epochs to the build lane: the lane-only ones as one group, the others as another. */
		void Dispatch()
		{
			auto& host = *desc.host;
			// Each group is attached or staged at the poll that finds all its builds finished.
			for (const bool laneOnlyGroup : { true, false }) {
				std::vector<Job> jobs;
				const auto group = ++groupSerial;
				for (std::size_t i = 0; i < host.GraphCount(); ++i) {
					const auto epoch = host.EpochAt(i);
					if (laneOnly.contains(epoch) != laneOnlyGroup || !Due(epoch))
						continue;
					jobs.push_back(MakeJob(epoch, group));
					inFlight.insert(epoch);
				}
				if (jobs.empty())
					continue;
				groups[group].remaining = jobs.size();
				for (auto& job : jobs)
					if (!lane.Post(std::move(job)))
						return;  // stopped: the manager is shutting down
			}
		}

		void LogWindow()
		{
			if (window.Empty())
				return;
			logger::info("[ORG] build manager, last {} frames: {} built off the render thread (mean {:.2f} ms, max {:.2f} ms), {} attached (mean {:.3f} ms, "
						 "max {:.3f} ms on the render thread), {} superseded, {} rejected, {} failed, {} built on the render thread, {} stale alias rebuilds",
				kStatsFrames, window.builds, window.builds ? window.buildMs / static_cast<double>(window.builds) : 0.0, window.buildMaxMs, window.attached,
				window.attached ? window.attachMs / static_cast<double>(window.attached) : 0.0, window.attachMaxMs, window.superseded, window.rejected,
				window.failed, window.synchronous, window.stale);
			window = {};
		}

		void Stop()
		{
			{
				std::scoped_lock lock(mutex);
				if (stop)
					return;
				stop = true;
			}
			// Drops the queued builds and waits for a running one.
			lane.Stop();
			// Never attached, so never run. Dropped here on the owner thread.
			finished.clear();
			groups.clear();
			inFlight.clear();
		}
	};

	BuildManager::BuildManager(Desc a_desc) :
		impl(std::make_unique<Impl>(std::move(a_desc)))
	{
		logger::info(
			"[ORG] build manager: structural builds run on a lane of their own and are attached at frame boundaries; {} staged and attached "
			"with a frame revision or a fragment, {} of them never built at a submission",
			impl->staged.size(), impl->laneOnly.size());
	}

	BuildManager::~BuildManager()
	{
		Shutdown();
	}

	void BuildManager::Shutdown()
	{
		if (impl)
			impl->Stop();
	}

	void BuildManager::Request(std::uint32_t a_epoch, std::string_view a_reason)
	{
		impl->Submit({ PendingRequest::Kind::Epoch, a_epoch, {}, std::string(a_reason) });
	}

	void BuildManager::RequestAll(std::string_view a_reason)
	{
		impl->Submit({ PendingRequest::Kind::All, 0, {}, std::string(a_reason) });
	}

	void BuildManager::RequestFor(std::string_view a_id, std::string_view a_reason)
	{
		impl->Submit({ PendingRequest::Kind::For, 0, std::string(a_id), std::string(a_reason) });
	}

	void BuildManager::Change(std::function<void()> a_change, std::string_view a_reason)
	{
		PendingRequest request{ PendingRequest::Kind::Change, 0, {}, std::string(a_reason) };
		request.change = std::move(a_change);
		impl->Submit(std::move(request));
	}

	std::string BuildManager::AttachStaged(std::span<const std::uint32_t> a_epochs)
	{
		auto& m = *impl;
		auto& host = *m.desc.host;
		const auto begin = Clock::now();
		const auto result = host.AttachStagedEpochGraphs(a_epochs);
		if (!result.attached) {
			RenderGraph::Frame::Boundary::NoteRefusal("revision", org::GraphHost::AttachRefusalName(result.refusal));
			return result.reason.empty() ? std::string("refused") : result.reason;
		}
		const double ms = MillisecondsSince(begin);
		m.window.attachMs += ms;
		m.window.attachMaxMs = (std::max)(m.window.attachMaxMs, ms);
		for (const auto epoch : a_epochs) {
			RenderGraph::Frame::Boundary::NoteAttach(m.Label(epoch));
			++m.window.attached;
			auto deferred = std::move(m.stagedAttach[epoch]);
			m.stagedAttach.erase(epoch);
			for (auto& fn : deferred)
				if (fn)
					fn();
		}
		return {};
	}

	void BuildManager::SetPollListener(std::function<void(std::span<const std::uint32_t>)> a_listener)
	{
		impl->pollListener = std::move(a_listener);
	}

	void BuildManager::SetBuildInput(std::function<std::shared_ptr<const void>()> a_input)
	{
		impl->input = std::move(a_input);
	}

	std::shared_ptr<const void> BuildManager::OwnerInput() const
	{
		return impl->OnOwner() ? impl->TakeInput() : nullptr;
	}

	bool BuildManager::Poll(std::string_view a_point)
	{
		auto& m = *impl;
		const Frame::Boundary::Caller caller(a_point);
		m.owner.store(std::this_thread::get_id(), std::memory_order_release);
		m.ApplyPending();
		std::vector<std::uint32_t> changedEpochs;
		const bool changed = m.AttachFinished(changedEpochs);
		m.RequestStale();
		m.Dispatch();
		// The listener hears of every poll, since any may stage its epochs.
		if (m.pollListener)
			m.pollListener(changedEpochs);
		return changed;
	}

	bool BuildManager::Settled() const
	{
		return impl->Settled();
	}

	bool BuildManager::UnstagedRebuildRequested() const
	{
		auto& host = *impl->desc.host;
		for (std::size_t i = 0; i < host.GraphCount(); ++i)
			if (const auto epoch = host.EpochAt(i); host.RebuildRequested(epoch) && !impl->staged.contains(epoch))
				return true;
		return false;
	}

	bool BuildManager::BuildSetupGraphs(std::string_view a_caller)
	{
		auto& m = *impl;
		if (m.worldStarted) {
			(void)Poll(a_caller);
			return false;
		}
		m.owner.store(std::this_thread::get_id(), std::memory_order_release);
		m.ApplyPending();
		std::scoped_lock lock(m.buildMutex);
		(void)m.desc.host->BuildIfRequested();
		return true;
	}

	bool BuildManager::Idle() const
	{
		return impl->groups.empty();
	}

	void BuildManager::NoteFrameEnd()
	{
		auto& m = *impl;
		++m.frames;
		if (!m.worldStarted && CensusWindow::Current() == CensusWindow::Kind::World) {
			m.worldStarted = true;
			logger::info("[ORG] build manager: first world frame; from here a graph built on the render thread is logged as a warning");
		}
		if (m.frames % kStatsFrames == 0)
			m.LogWindow();
	}

	void BuildManager::WarnSynchronous(std::string_view a_what, std::string_view a_caller)
	{
		impl->WarnSynchronous(a_what, a_caller);
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
			if (!t_context)
				return false;
			if (a_attached)
				t_context->attached.push_back(std::move(a_attached));
			if (a_failed)
				t_context->failed.push_back(std::move(a_failed));
			return true;
		}

		bool DeferStaged(std::function<void()> a_staged)
		{
			if (!t_context)
				return false;
			if (a_staged)
				t_context->staged.push_back(std::move(a_staged));
			return true;
		}

		std::shared_ptr<const void> Input()
		{
			if (t_context)
				return t_context->input;
			// The host's own builds on the owner thread (an epoch's first build, setup) read the owner's state now.
			auto* manager = Manager();
			return manager ? manager->OwnerInput() : nullptr;
		}
	}
}


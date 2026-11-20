#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/BuildManager.h"

#	include "RenderGraph/CensusWindow.h"
#	include "RenderGraph/Frame/BoundaryCheck.h"
#	include "RenderGraph/Frame/Recipe.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Resources/ResourceIdentifier.h>

#	include <algorithm>
#	include <atomic>
#	include <chrono>
#	include <condition_variable>
#	include <deque>
#	include <map>
#	include <mutex>
#	include <thread>
#	include <unordered_map>
#	include <unordered_set>

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
			bool external = false;  // the external builder's (DCLF's), which are staged too
			std::vector<std::unique_ptr<Done>> done;
		};

		/** @brief Checked by a posted external build before it runs, since the manager may be gone by then. */
		struct ExternalLife
		{
			std::mutex mutex;
			std::condition_variable idle;
			bool stopped = false;
			std::uint32_t running = 0;
		};

		Desc desc;
		std::unordered_set<std::uint32_t> ownerBuilt;
		std::unordered_set<std::uint32_t> external;
		std::unordered_set<std::uint32_t> staged;
		// Owner thread. Run by AttachStaged.
		std::unordered_map<std::uint32_t, std::vector<std::function<void()>>> stagedAttach;
		ExternalBuilder externalBuilder;  // owner thread
		std::shared_ptr<ExternalLife> externalLife = std::make_shared<ExternalLife>();

		// Build thread.
		std::thread thread;
		mutable std::mutex mutex;  // queue, finished, stop
		std::condition_variable wake;
		std::deque<Job> queue;
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

		explicit Impl(Desc a_desc) :
			desc(std::move(a_desc)), ownerBuilt(desc.ownerBuilt.begin(), desc.ownerBuilt.end()), external(desc.external.begin(), desc.external.end()),
			staged(desc.staged.begin(), desc.staged.end())
		{
			thread = std::thread([this] { Run(); });
		}

		std::string Label(std::uint32_t a_epoch) const
		{
			return desc.label ? desc.label(a_epoch) : fmt::format("epoch {}", a_epoch);
		}

		bool OnOwner() const { return std::this_thread::get_id() == owner.load(std::memory_order_acquire); }

		void Run()
		{
			for (;;) {
				Job job;
				{
					std::unique_lock lock(mutex);
					wake.wait(lock, [&] { return stop || !queue.empty(); });
					if (stop)
						return;
					job = std::move(queue.front());
					queue.pop_front();
				}
				auto done = Build(std::move(job));
				std::scoped_lock lock(mutex);
				finished.push_back(std::move(done));
			}
		}

		std::unique_ptr<Done> Build(Job a_job)
		{
			auto done = std::make_unique<Done>();
			done->job = std::move(a_job);
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

		std::shared_ptr<void> OwnerScope() const
		{
			return externalBuilder.ownerScope ? externalBuilder.ownerScope() : nullptr;
		}

		/** @brief Whether the owner thread builds a_epoch, which includes an external one with no builder to post to. */
		bool BuiltOnOwner(std::uint32_t a_epoch) const
		{
			return ownerBuilt.contains(a_epoch) || (external.contains(a_epoch) && !externalBuilder.post);
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

		/** @brief The epoch is asked for, has a graph to keep running, and is not being built or known to fail. */
		bool Due(std::uint32_t a_epoch) const
		{
			auto& host = *desc.host;
			if (!host.RebuildRequested(a_epoch) || inFlight.contains(a_epoch))
				return false;
			// No graph yet means the host builds it at its first submission.
			if (!host.Graph(a_epoch))
				return false;
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
		bool Finish(Done& a_done, bool a_synchronous)
		{
			auto& host = *desc.host;
			const auto epoch = a_done.job.epoch;
			if (!a_done.graph || !a_done.graph->Built()) {
				Fail(a_done, a_done.graph ? a_done.graph->Error() : a_done.error);
				return false;
			}
			// Read now, since an attach takes the graph.
			const double buildMs = a_done.graph->BuildMilliseconds();
			if (!a_synchronous) {
				++window.builds;
				window.buildMs += buildMs;
				window.buildMaxMs = (std::max)(window.buildMaxMs, buildMs);
			}
			// Requested again since it was handed out, so drop it and rebuild.
			// After too many in a row it goes in anyway, since it is newer than the running graph.
			if (!a_synchronous && host.RebuildRequests(epoch) != a_done.job.requests && supersededInARow[epoch] < kMaxSupersededInARow) {
				++supersededInARow[epoch];
				++window.superseded;
				return false;
			}
			const auto begin = Clock::now();
			// A staged epoch is recorded by the host's thread first, and a frame revision (or DCLF's snapshot adoption) attaches it.
			// A synchronous build of one is staged too (DCLF's with CS_DCLF_ASYNC_BUILDS=0).
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
					logger::info("[ORG] build manager: {} staged as build {}: built in {:.2f} ms {}, staged in {:.3f} ms, {} frames after it was "
								 "asked for; it attaches with a frame revision; why: {}",
						Label(epoch), stagedResult.generation, buildMs, a_synchronous ? "on the render thread" : "off the render thread", MillisecondsSince(begin),
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
				logger::info("[ORG] build manager: {} attached{}: built in {:.2f} ms {}, attached in {:.3f} ms on the render thread, {} frames after "
							 "it was asked for; why: {}",
					Label(epoch), attachLogs > 200 ? fmt::format(" (attach {})", attachLogs) : std::string(), buildMs,
					a_synchronous ? "on the render thread" : "off the render thread", attachMs, frames - a_done.job.frame, a_done.job.reason);
			return true;
		}

		/**
		* @brief Attaches or stages every group whose builds have all finished. a_external is set when an external group's build
		* was staged. The external builder's epochs are staged, so none of them attaches here.
		*/
		bool AttachFinished(bool* a_external = nullptr)
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
					const bool one = Finish(*done, false);
					attached = one || attached;
					if (one && it->second.external && a_external)
						*a_external = true;
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

		/** @brief Builds the owner thread's epochs now. Staged ones are staged, not attached. */
		bool BuildOwnerEpochs(std::string_view a_caller)
		{
			auto& host = *desc.host;
			bool built = false;
			std::shared_ptr<void> scope;
			for (std::size_t i = 0; i < host.GraphCount(); ++i) {
				const auto epoch = host.EpochAt(i);
				if (!BuiltOnOwner(epoch) || !Due(epoch))
					continue;
				WarnSynchronous(Label(epoch), a_caller);
				++window.synchronous;
				if (!scope)
					scope = OwnerScope();
				auto done = Build(MakeJob(epoch, 0));
				built = Finish(*done, true) || built;
			}
			return built;
		}

		/** @brief Hands one group's external epochs to the external builder. Returns the jobs it refused. */
		std::vector<Job> PostExternal(std::vector<Job> a_jobs)
		{
			std::vector<Job> refused;
			for (auto& job : a_jobs) {
				auto life = externalLife;
				auto run = [this, life, job]() mutable {
					{
						std::scoped_lock lock(life->mutex);
						if (life->stopped)
							return;
						++life->running;
					}
					auto done = Build(std::move(job));
					{
						std::scoped_lock lock(mutex);
						finished.push_back(std::move(done));
					}
					std::scoped_lock lock(life->mutex);
					--life->running;
					life->idle.notify_all();
				};
				if (!externalBuilder.post(std::move(run)))
					refused.push_back(job);
			}
			return refused;
		}

		bool Settled() const
		{
			auto& host = *desc.host;
			for (std::size_t i = 0; i < host.GraphCount(); ++i) {
				const auto epoch = host.EpochAt(i);
				if (!ownerBuilt.contains(epoch) && !external.contains(epoch))
					continue;
				if (inFlight.contains(epoch))
					return false;
				if (!host.Graph(epoch) || !host.RebuildRequested(epoch))
					continue;
				const auto failed = failedAt.find(epoch);
				if (failed == failedAt.end() || failed->second != host.RebuildRequests(epoch))
					return false;
			}
			return true;
		}

		void Dispatch(std::string_view a_point)
		{
			auto& host = *desc.host;
			// The external builder's epochs form their own group, staged at the poll that finds it finished.
			if (externalBuilder.post) {
				std::vector<Job> posted;
				const auto group = ++groupSerial;
				for (std::size_t i = 0; i < host.GraphCount(); ++i) {
					const auto epoch = host.EpochAt(i);
					if (!external.contains(epoch) || !Due(epoch))
						continue;
					posted.push_back(MakeJob(epoch, group));
					inFlight.insert(epoch);
				}
				if (!posted.empty()) {
					auto& entry = groups[group];
					entry.external = true;
					entry.remaining = posted.size();
					// Refused jobs are built here on the owner thread and staged with the group.
					auto refused = PostExternal(std::move(posted));
					if (!refused.empty()) {
						auto scope = OwnerScope();
						for (auto& job : refused) {
							WarnSynchronous(Label(job.epoch), fmt::format("{} (the external builder refused it)", a_point));
							++window.synchronous;
							auto done = Build(std::move(job));
							std::scoped_lock lock(mutex);
							finished.push_back(std::move(done));
						}
					}
				}
			}
			std::vector<Job> jobs;
			const auto group = ++groupSerial;
			for (std::size_t i = 0; i < host.GraphCount(); ++i) {
				const auto epoch = host.EpochAt(i);
				if (ownerBuilt.contains(epoch) || external.contains(epoch) || !Due(epoch))
					continue;
				jobs.push_back(MakeJob(epoch, group));
				inFlight.insert(epoch);
			}
			if (jobs.empty())
				return;
			groups[group].remaining = jobs.size();
			{
				std::scoped_lock lock(mutex);
				for (auto& job : jobs)
					queue.push_back(std::move(job));
			}
			wake.notify_one();
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
				if (stop && !thread.joinable())
					return;
				stop = true;
				queue.clear();
			}
			wake.notify_all();
			if (thread.joinable())
				thread.join();
			// Posted builds that have not started return at once. Running ones are waited for.
			{
				std::unique_lock lock(externalLife->mutex);
				externalLife->stopped = true;
				externalLife->idle.wait(lock, [&] { return externalLife->running == 0; });
			}
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
			"[ORG] build manager: structural builds run on a thread of their own and are attached at frame boundaries; {} owner-built epochs, "
			"{} built by an external builder, {} staged and attached with a frame revision or DCLF's snapshot",
			impl->ownerBuilt.size(), impl->external.size(), impl->staged.size());
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

	bool BuildManager::SwapPipelines(std::string_view a_reason)
	{
		auto& m = *impl;
		m.owner.store(std::this_thread::get_id(), std::memory_order_release);
		m.ApplyPending();
		WarnSynchronous(fmt::format("every graph, for the pipeline swap ({})", a_reason), "revision");
		++m.window.synchronous;
		m.desc.host->RequestRebuild(a_reason);
		auto scope = m.OwnerScope();
		std::scoped_lock lock(m.buildMutex);
		return m.desc.host->BuildIfRequested();
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

	void BuildManager::SetExternalBuilder(ExternalBuilder a_builder)
	{
		impl->externalBuilder = std::move(a_builder);
		logger::info("[ORG] build manager: external builder set ({} epochs; {}, staged)", impl->external.size(),
			impl->externalBuilder.post ? "built on its worker" : "built on the render thread");
	}

	bool BuildManager::Poll(std::string_view a_point)
	{
		auto& m = *impl;
		const Frame::Boundary::Caller caller(a_point);
		m.owner.store(std::this_thread::get_id(), std::memory_order_release);
		m.ApplyPending();
		bool changed = false;
		bool externalStaged = false;
		changed = m.AttachFinished(&externalStaged) || changed;
		m.RequestStale();
		const bool builtHere = m.BuildOwnerEpochs(a_point);
		changed = builtHere || changed;
		m.Dispatch(a_point);
		// The external builder hears of every poll, since any may stage its epochs.
		if (m.externalBuilder.boundary)
			m.externalBuilder.boundary(externalStaged || builtHere);
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
		auto scope = m.OwnerScope();
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

		void Request(std::uint32_t a_epoch, std::string_view a_reason)
		{
			if (auto* manager = Manager()) {
				Frame::Boundary::NoteBuildRequest(a_reason);
				manager->Request(a_epoch, a_reason);
			}
		}

		void RequestAll(std::string_view a_reason)
		{
			if (auto* manager = Manager()) {
				Frame::Boundary::NoteBuildRequest(a_reason);
				manager->RequestAll(a_reason);
			}
		}

		void RequestFor(std::string_view a_id, std::string_view a_reason)
		{
			if (auto* manager = Manager()) {
				Frame::Boundary::NoteBuildRequest(a_reason);
				manager->RequestFor(a_id, a_reason);
			}
		}

		void Change(std::function<void()> a_change, std::string_view a_reason)
		{
			if (auto* manager = Manager()) {
				Frame::Boundary::NoteBuildRequest(a_reason);
				manager->Change(std::move(a_change), a_reason);
			}
			else if (a_change)
				a_change();
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

		std::string AttachStaged(std::span<const std::uint32_t> a_epochs)
		{
			auto* manager = Manager();
			if (!manager)
				return a_epochs.empty() ? std::string() : std::string("the build manager is gone");
			return manager->AttachStaged(a_epochs);
		}

		bool InManagedBuild()
		{
			return t_context != nullptr;
		}
	}
}

#else  // !CS_HAS_RENDER_GRAPH

#	include "RenderGraph/BuildManager.h"

namespace RenderGraph::Builds
{
	void Request(std::uint32_t, std::string_view) {}
	void RequestAll(std::string_view) {}
	void RequestFor(std::string_view, std::string_view) {}
	void Change(std::function<void()> a_change, std::string_view)
	{
		if (a_change)
			a_change();
	}
	bool Defer(std::function<void()>, std::function<void(const std::string&)>) { return false; }
	bool DeferStaged(std::function<void()>) { return false; }
	std::string AttachStaged(std::span<const std::uint32_t> a_epochs) { return a_epochs.empty() ? std::string() : std::string("no render graph"); }
	bool InManagedBuild() { return false; }
}

#endif

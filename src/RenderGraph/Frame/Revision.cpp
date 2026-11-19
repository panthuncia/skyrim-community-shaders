#include "RenderGraph/Frame/Revision.h"

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)

#	include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"
#	include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#	include "RenderGraph/Frame/BoundaryCheck.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Frame/RevisionRecordings.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	if !defined(CS_RELEASE_PROFILE)
#		include "Testing/Testing.h"
#	endif

#	include <ORGModuleServices/Async/RevisionAssembly.h>
#	include <ORGModuleServices/Async/SerializedTaskPump.h>
#	include <ORGModuleServices/BackingPool.h>
#	include <ORGModuleServices/ComposedVersions.h>
#	include <ORGModuleServices/GrowableBufferSet.h>
#	include <ORGModuleServices/VersionedTextureGrowth.h>
#	include <OpenRenderGraph/GraphHost.h>
#	include <Resources/VersionedTexture.h>

#	include <algorithm>
#	include <atomic>
#	include <chrono>
#	include <deque>
#	include <map>
#	include <mutex>
#	include <optional>
#	include <string>
#	include <vector>

namespace RenderGraph::Frame::Revision
{
	namespace
	{
		using GraphResources::Extent;

		// The revision's fragments (plan, step 4c). Inputs come with step 5, buffers and builds with step 4e.
		constexpr std::uint32_t kRevisionInputs = 0;
		constexpr std::uint32_t kRevisionTextures = 1;
		constexpr std::uint32_t kRevisionBuffers = 2;
		constexpr std::uint32_t kRevisionBuilds = 3;
		constexpr std::uint32_t kRevisionRecordings = 4;
		constexpr std::uint32_t kRevisionSlots = 5;

		// The starvation rule (plan 7.8): recordings dropped and asked for again this often with no new wish is a schedule bug.
		constexpr std::uint64_t kRevisionStarvedAfter = 4 * RenderGraphRuntime::kGameFramesInFlight;

		/** @brief A twin's newest size wish, held by its Alive() token since the twin may go first. */
		struct RevisionWish
		{
			std::weak_ptr<const void> alive;
			GraphResources::Resource* twin = nullptr;
			Extent extent{};
			std::uint64_t frame = 0;  // the frame of the first wish since the twin last had its wished size
		};

		/** @brief The value of the recordings fragment. */
		struct RevisionMemberRecordings
		{
			std::shared_ptr<FrameAnchors::RevisionRecordings> recordings;
		};

		/** @brief One revision: what the render thread planned, what the lane made of it, and its recordings. */
		struct PreparedRevision
		{
			// Written on the render thread before the lane takes it.
			std::uint64_t serial = 0;
			std::vector<std::shared_ptr<const GraphResources::PreparePlan>> plans;
			std::vector<GraphResources::Resource*> twins;
			std::vector<std::weak_ptr<const void>> alive;
			std::vector<std::string> names;
			std::vector<Extent> from;  // each twin's extent when planned
			std::vector<Extent> to;
			std::uint64_t firstWish = 0;
			std::uint64_t planned = 0;  // the frame it was handed to the lane
			std::shared_ptr<org::runtime::IUploadService> uploads;
			org::GraphHost* host = nullptr;
			// Written on the lane, then done.
			std::vector<GraphResources::PreparedVersion> versions;
			std::shared_ptr<const org::services::TextureVersionsMade> made;
			std::vector<std::uint32_t> closure;
			std::string error;
			double milliseconds = 0.0;
			std::atomic<bool> done{ false };
			// Render thread, once the lane is done.
			std::shared_ptr<const org::services::ComposedVersions> versionSet;
			std::shared_ptr<FrameAnchors::RevisionRecordings> recordings;
			std::vector<std::uint32_t> others;  // closure epochs that are not CS stages
			std::uint64_t askedAgain = 0;
			std::uint32_t recordingCount = 0;
		};

		struct RevisionStats
		{
			std::uint64_t prepared = 0, adopted = 0, superseded = 0, resized = 0, askedAgain = 0;
			std::uint32_t maxMembers = 0, maxRecordings = 0;
			std::vector<std::uint64_t> wishFrames;
		};

		struct Coordinator
		{
			bool installed = false;
			// The lane's hand-off: the render thread posts one revision, the lane takes it.
			std::mutex laneMutex;
			std::shared_ptr<PreparedRevision> laneJob;
			org::async::SerializedTaskPump* pump = nullptr;
			// Render thread from here.
			std::map<GraphResources::Resource*, RevisionWish> wishes;
			std::map<GraphResources::Resource*, RevisionWish> resizes;  // wishes no version serves
			std::uint64_t wishSerial = 0;
			std::uint64_t plannedSerial = 0;
			std::uint64_t askedAgainSinceWish = 0;
			std::uint64_t serials = 0;
			std::shared_ptr<PreparedRevision> preparing;
			std::shared_ptr<PreparedRevision> adopted;
			std::deque<std::pair<std::uint64_t, std::shared_ptr<PreparedRevision>>> retiring;
			std::unique_ptr<org::async::RevisionAssembler> assembler = std::make_unique<org::async::RevisionAssembler>(kRevisionSlots);
			std::uint64_t adoptedLines = 0;
			// The census, read by LogStats on any thread.
			std::mutex statsMutex;
			RevisionStats window;
			RevisionStats session;
		};

		Coordinator& GetCoordinator()
		{
			// Never destroyed: the lane may still finish a preparation during process teardown.
			static auto* coordinator = new Coordinator;
			return *coordinator;
		}

		[[noreturn]] void FailRevision(std::string_view a_why)
		{
			const auto message = fmt::format("Community Shaders: a frame revision failed: {}", a_why);
			logger::critical("[ORG] frame revision: {}", message);
			spdlog::default_logger()->flush();
			stl::report_and_fail(message);
		}

		std::string ExtentText(const Extent& a_extent)
		{
			return a_extent[2] > 1 ? fmt::format("{}x{}x{}", a_extent[0], a_extent[1], a_extent[2]) : fmt::format("{}x{}", a_extent[0], a_extent[1]);
		}

		/** @brief An epoch that is not a CS stage, named by its segment. */
		std::string EpochText(std::uint32_t a_epoch)
		{
			using Segment = RenderGraphRuntime::Segment;
			for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(Segment::Count); ++s)
				if (RenderGraphRuntime::EpochOf(static_cast<Segment>(s)) == a_epoch)
					return RenderGraphRuntime::IsDclfSegment(static_cast<Segment>(s)) ? fmt::format("DCLF epoch {} (segment {})", a_epoch, s) :
					                                                                    fmt::format("epoch {} (segment {})", a_epoch, s);
			return fmt::format("epoch {}", a_epoch);
		}

		/** @brief The lane's work: every version made with its memory, the zero fills queued, and the closure. Never on the render thread. */
		void DrainRevisionLane()
		{
			auto& coordinator = GetCoordinator();
			std::shared_ptr<PreparedRevision> job;
			{
				const std::lock_guard lock(coordinator.laneMutex);
				job = std::move(coordinator.laneJob);
			}
			if (!job)
				return;
			ZoneScopedN("CS.Frame.Revision.Prepare");
			const auto start = std::chrono::steady_clock::now();
			try {
				std::vector<org::services::TextureVersionRequest> requests;
				requests.reserve(job->plans.size());
				for (const auto& plan : job->plans) {
					auto prepared = GraphResources::Resource::Prepare(*plan);
					org::services::TextureVersionRequest request;
					request.slot = prepared.slot;
					request.image = prepared.resource;
					request.extent = { prepared.extent[0], prepared.extent[1], prepared.extent[2] };
					request.contents = prepared.contents == GraphResources::Contents::Zero ? org::services::TextureContents::Zero :
					                                                                          org::services::TextureContents::Undefined;
					// An exported version's image lives as long as the version.
					request.owner = prepared.exported;
					requests.push_back(std::move(request));
					job->versions.push_back(std::move(prepared));
				}
				job->made = std::make_shared<const org::services::TextureVersionsMade>(org::services::MakeTextureVersions(requests, job->uploads));
				std::vector<const org::IVersionedResource*> slots;
				for (const auto& version : job->versions)
					slots.push_back(version.slot.get());
				job->closure = job->host->EpochsDeclaring(slots);
			} catch (const std::exception& e) {
				job->error = e.what();
			}
			job->milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			job->done.store(true, std::memory_order_release);
		}

		org::async::SerializedTaskPump& RevisionLane()
		{
			auto& coordinator = GetCoordinator();
			if (!coordinator.pump) {
				// A lane on DCLF's executor, the one the runtime already has (plan Q3). Its preparation pool takes the work.
				static const org::async::Scope scope = DCLF::SceneScheduler::Executor().CreateScope("CS frame revision");
				coordinator.pump = DCLF::SceneScheduler::MakeLanePump(DCLF::SceneScheduler::Executor(), scope, DCLF::SceneExecutor::Preparation,
					org::async::TaskDispatch::Controlled, "CS frame revision", [] { DrainRevisionLane(); },
					[] { logger::critical("[ORG] frame revision: the revision lane stopped (its task was refused)"); });
			}
			return *coordinator.pump;
		}

		/** @brief The hook RequestSize calls on every change of a twin's wish. Render thread. */
		void OnSizeWish(GraphResources::Resource& a_twin, const std::optional<Extent>& a_extent)
		{
			auto& coordinator = GetCoordinator();
			++coordinator.wishSerial;
			// A new event: drops asked again before it are not starvation.
			coordinator.askedAgainSinceWish = 0;
			coordinator.resizes.erase(&a_twin);
			if (!a_extent) {
				coordinator.wishes.erase(&a_twin);
				return;
			}
			const auto frame = Driver::Current().frame;
			if (!a_twin.Preparable(*a_extent)) {
				coordinator.wishes.erase(&a_twin);
				coordinator.resizes[&a_twin] = { a_twin.Alive(), &a_twin, *a_extent, frame };
				return;
			}
			auto& wish = coordinator.wishes[&a_twin];
			const bool standing = wish.twin == &a_twin && !wish.alive.expired();
			wish = { a_twin.Alive(), &a_twin, *a_extent, standing ? wish.frame : frame };
		}

		/** @brief Wishes no version serves take today's path: the twin is made again now, inside the adoption point. */
		void ApplyResizes(Coordinator& a_coordinator)
		{
			for (auto& [twin, wish] : std::exchange(a_coordinator.resizes, {})) {
				if (wish.alive.expired() || twin->CurrentExtent() == wish.extent)
					continue;
				twin->ResizeTo(wish.extent);
				if (!*twin)
					FailRevision(fmt::format("{} could not be made again at {} at a frame boundary", twin->Name(), ExtentText(wish.extent)));
				const std::lock_guard lock(a_coordinator.statsMutex);
				++a_coordinator.window.resized;
				++a_coordinator.session.resized;
			}
		}

		/** @brief Hands the standing wishes to the lane as the next revision. Only one is prepared at a time. */
		void StartPreparation(Coordinator& a_coordinator, std::uint64_t a_frame)
		{
			if (a_coordinator.preparing || a_coordinator.wishes.empty() || a_coordinator.plannedSerial == a_coordinator.wishSerial)
				return;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return;
			auto job = std::make_shared<PreparedRevision>();
			job->firstWish = UINT64_MAX;
			for (auto it = a_coordinator.wishes.begin(); it != a_coordinator.wishes.end();) {
				auto& [twin, wish] = *it;
				if (wish.alive.expired() || twin->CurrentExtent() == wish.extent) {
					it = a_coordinator.wishes.erase(it);
					continue;
				}
				auto plan = twin->PlanPrepare(wish.extent);
				if (!plan) {
					a_coordinator.resizes[twin] = wish;
					it = a_coordinator.wishes.erase(it);
					continue;
				}
				job->plans.push_back(std::move(plan));
				job->twins.push_back(twin);
				job->alive.push_back(wish.alive);
				job->names.push_back(twin->Name());
				job->from.push_back(twin->CurrentExtent());
				job->to.push_back(wish.extent);
				job->firstWish = (std::min)(job->firstWish, wish.frame);
				++it;
			}
			a_coordinator.plannedSerial = a_coordinator.wishSerial;
			if (job->plans.empty())
				return;
			job->serial = ++a_coordinator.serials;
			job->planned = a_frame;
			job->uploads = host->RetainUploads();
			job->host = host;
			a_coordinator.preparing = job;
			{
				const std::lock_guard lock(a_coordinator.laneMutex);
				a_coordinator.laneJob = std::move(job);
			}
			if (!RevisionLane().Notify())
				FailRevision("the revision lane refused its work");
		}

		/** @brief Whether every twin of the revision is still the one it was planned for. Render thread. */
		bool StillPlanned(const PreparedRevision& a_revision, std::string& a_why)
		{
			for (std::size_t i = 0; i < a_revision.twins.size(); ++i) {
				auto* twin = a_revision.twins[i];
				if (a_revision.alive[i].expired()) {
					a_why = fmt::format("{} was destroyed", a_revision.names[i]);
					return false;
				}
				if (twin->Versioned() != a_revision.versions[i].slot || twin->CurrentExtent() != a_revision.from[i]) {
					a_why = fmt::format("{} was made again since", a_revision.names[i]);
					return false;
				}
			}
			return true;
		}

		/** @brief Drops a revision that can never be adopted. Its twins' wishes stand and are prepared again. Supersession is never an error. */
		void Supersede(Coordinator& a_coordinator, std::string_view a_why)
		{
			logger::info("[ORG] frame revision {}: superseded before adoption ({}); its wishes are prepared again", a_coordinator.preparing->serial, a_why);
			a_coordinator.preparing.reset();
			a_coordinator.plannedSerial = 0;
			const std::lock_guard lock(a_coordinator.statsMutex);
			++a_coordinator.window.superseded;
			++a_coordinator.session.superseded;
		}

		void ReleaseRetired(Coordinator& a_coordinator)
		{
			const auto completed = DCLF::FrameRetirement()->Completed();
			while (!a_coordinator.retiring.empty() && a_coordinator.retiring.front().first <= completed)
				a_coordinator.retiring.pop_front();
		}

		/** @brief Adopts a ready revision whole. Render thread, inside the adoption point. */
		void Adopt(Coordinator& a_coordinator, std::uint64_t a_frame)
		{
			auto revision = std::move(a_coordinator.preparing);
			auto& assembler = *a_coordinator.assembler;
			// Sealed once complete, so it is selected in this Begin. Inputs, buffers and builds stay empty until steps 5 and 4e.
			auto draft = assembler.Begin();
			draft.Set(kRevisionInputs, nullptr);
			draft.Set(kRevisionBuffers, nullptr);
			draft.Set(kRevisionBuilds, nullptr);
			draft.Set(kRevisionTextures, revision->made->fragment);
			auto memberValue = std::make_shared<RevisionMemberRecordings>();
			memberValue->recordings = revision->recordings;
			draft.Set(kRevisionRecordings, org::async::RevisionFragment::MakeReady(std::move(memberValue), { { kRevisionTextures, revision->made->fragment } }));
			assembler.Seal(std::move(draft));
			const auto collected = assembler.Collect();
			if (!collected.failures.empty())
				FailRevision(fmt::format("revision {} failed to assemble", revision->serial));
			if (assembler.TrySelect() != org::async::RevisionAssembler::Selection::Selected)
				FailRevision(fmt::format("revision {} was complete but not selected", revision->serial));
			const auto lease = assembler.AcquireActive();
			const auto named = lease->Get<org::services::NamedVersions>(kRevisionTextures);
			const auto recorded = lease->Get<RevisionMemberRecordings>(kRevisionRecordings);
			if (!named || !recorded)
				FailRevision(fmt::format("revision {} was selected without its versions or recordings", revision->serial));
			{
				const Boundary::Caller caller("revision");
				named->AdoptAll();
				for (std::size_t i = 0; i < revision->twins.size(); ++i)
					revision->twins[i]->AdoptPrepared(revision->versions[i]);
			}
			// Per-frame tickets (DCLF's, the anchors') prepared before resolved the old versions. Revision-driven stages need no call.
			revision->host->NoteNewVersions();
			if (recorded->recordings)
				FrameAnchors::InstallRevisionSchedules(*recorded->recordings);
			// The last revision's versions and recordings go once the GPU is past every frame submitted with them.
			if (a_coordinator.adopted)
				a_coordinator.retiring.emplace_back(DCLF::SubmittedRetirementPoint(), std::move(a_coordinator.adopted));
			for (std::size_t i = 0; i < revision->twins.size(); ++i)
				if (const auto it = a_coordinator.wishes.find(revision->twins[i]); it != a_coordinator.wishes.end() && it->second.extent == revision->to[i])
					a_coordinator.wishes.erase(it);
			const auto members = revision->recordings ? FrameAnchors::RevisionMembers(*revision->recordings) : std::vector<std::string>{};
			const std::uint64_t wishFrames = a_frame > revision->firstWish ? a_frame - revision->firstWish : 0;
			{
				const std::lock_guard lock(a_coordinator.statsMutex);
				for (auto* stats : { &a_coordinator.window, &a_coordinator.session }) {
					++stats->adopted;
					stats->maxMembers = (std::max)(stats->maxMembers, static_cast<std::uint32_t>(members.size()));
					stats->maxRecordings = (std::max)(stats->maxRecordings, revision->recordingCount);
					stats->wishFrames.push_back(wishFrames);
				}
			}
			if (++a_coordinator.adoptedLines <= 64 || a_coordinator.adoptedLines % 100 == 0) {
				std::string twins;
				for (std::size_t i = 0; i < revision->twins.size(); ++i)
					twins += fmt::format("{}{} {} -> {}", twins.empty() ? "" : ", ", revision->names[i], ExtentText(revision->from[i]), ExtentText(revision->to[i]));
				std::string stages;
				for (const auto& member : members)
					stages += fmt::format("{}{}", stages.empty() ? "" : ", ", member);
				std::string others;
				for (const auto epoch : revision->others)
					others += fmt::format("{}{}", others.empty() ? "" : ", ", EpochText(epoch));
				logger::info("[ORG] frame revision {}: adopted at frame {} with every member stage, {} frames after the first wish (prepared in {:.2f} ms on the "
							 "lane); twins [{}]; member stages [{}] with {} recordings; not members [{}]",
					revision->serial, a_frame, wishFrames, revision->milliseconds, twins, stages.empty() ? "none" : stages, revision->recordingCount,
					others.empty() ? "none" : others);
			}
			a_coordinator.adopted = std::move(revision);
		}

		/** @brief The revision in preparation, once the lane is done: recordings, then adoption. Render thread. */
		void Advance(Coordinator& a_coordinator, std::uint64_t a_frame)
		{
			auto& revision = *a_coordinator.preparing;
			if (!revision.error.empty())
				FailRevision(fmt::format("revision {} could not be prepared: {}", revision.serial, revision.error));
			std::string why;
			if (!StillPlanned(revision, why)) {
				Supersede(a_coordinator, why);
				return;
			}
			const auto& fragment = *revision.made->fragment;
			if (fragment.GetState() == org::async::RevisionFragment::State::Failed) {
				std::string error = "unknown";
				try {
					std::rethrow_exception(fragment.Error());
				} catch (const std::exception& e) {
					error = e.what();
				} catch (...) {
				}
				FailRevision(fmt::format("revision {}'s texture versions were not filled: {}", revision.serial, error));
			}
			// The zero fills run on the uploader's queue. Recordings wait for nothing, but the revision is adopted only once they are done.
			if (fragment.GetState() != org::async::RevisionFragment::State::Ready)
				return;
			if (!revision.recordings) {
				revision.versionSet = std::make_shared<const org::services::ComposedVersions>(
					std::initializer_list<std::shared_ptr<const org::IResourceVersions>>{ revision.made->named });
				revision.recordings = FrameAnchors::MakeRevisionRecordings(revision.closure, revision.versionSet, revision.versionSet->Serial(), revision.others);
				const std::lock_guard lock(a_coordinator.statsMutex);
				++a_coordinator.window.prepared;
				++a_coordinator.session.prepared;
			} else {
				// Builds may change which graphs declare the slots, so the closure is taken again until adoption.
				std::vector<const org::IVersionedResource*> slots;
				for (const auto& version : revision.versions)
					slots.push_back(version.slot.get());
				FrameAnchors::AddRevisionMembers(*revision.recordings, revision.host->EpochsDeclaring(slots), revision.others);
			}
			const auto progress = FrameAnchors::AdvanceRevisionRecordings(*revision.recordings, a_frame);
			revision.askedAgain += progress.askedAgain;
			a_coordinator.askedAgainSinceWish += progress.askedAgain;
			if (progress.askedAgain) {
				const std::lock_guard lock(a_coordinator.statsMutex);
				a_coordinator.window.askedAgain += progress.askedAgain;
				a_coordinator.session.askedAgain += progress.askedAgain;
			}
			if (!progress.ready && a_coordinator.askedAgainSinceWish > kRevisionStarvedAfter) {
				Driver::NoteTapError(Driver::TapError::Starved, static_cast<SegmentId>(progress.missing),
					fmt::format("frame revision {} (wishes from frame {}) has not been adopted: {} of its recordings were asked for again after they were "
								"dropped with no new wish since (last: {}); still missing [{}]{}",
						revision.serial, revision.firstWish, a_coordinator.askedAgainSinceWish, progress.dropReasons.empty() ? "none" : progress.dropReasons,
						progress.waiting, progress.deferred.empty() ? "" : fmt::format("; waiting for staged builds of [{}]", progress.deferred)));
				a_coordinator.askedAgainSinceWish = 0;
			}
			if (!progress.ready)
				return;
			revision.recordingCount = progress.recordings;
			Adopt(a_coordinator, a_frame);
		}

		std::string Percentiles(std::vector<std::uint64_t> a_values)
		{
			if (a_values.empty())
				return "none";
			std::ranges::sort(a_values);
			const auto at = [&](double a_fraction) { return a_values[static_cast<std::size_t>(a_fraction * static_cast<double>(a_values.size() - 1) + 0.5)]; };
			return fmt::format("{}/{}/{}", at(0.5), at(0.95), a_values.back());
		}
	}

	void Install()
	{
		auto& coordinator = GetCoordinator();
		coordinator.installed = true;
		GraphResources::Resource::SetSizeWishHook(&OnSizeWish);
#	if !defined(CS_RELEASE_PROFILE)
		Testing::RegisterExitReport([] { LogStats(); });
#	endif
		logger::info("[ORG] frame revision: twins' size wishes are prepared on the revision lane and adopted whole at a frame's Begin");
	}

	void AdoptLatest()
	{
		auto& coordinator = GetCoordinator();
		if (!coordinator.installed || !FrameAnchors::IsRenderOwner())
			return;
		ZoneScopedN("CS.Frame.Revision.Adopt");
		const std::uint64_t frame = Driver::Current().frame + 1;
		ReleaseRetired(coordinator);
		ApplyResizes(coordinator);
		if (coordinator.preparing && coordinator.preparing->done.load(std::memory_order_acquire))
			Advance(coordinator, frame);
		StartPreparation(coordinator, frame);
	}

	void LogStats()
	{
		auto& coordinator = GetCoordinator();
		if (!coordinator.installed)
			return;
		const std::lock_guard lock(coordinator.statsMutex);
		const auto& w = coordinator.window;
		const auto& s = coordinator.session;
		logger::info("[ORG] frame revision: prepared {}, adopted {}, superseded {}, members per revision (max {}), recordings per revision (max {}), frames "
					 "from first wish to adoption (p50/p95/max) {}; recordings asked again {}, resized at Begin without a version {}; session: prepared {}, "
					 "adopted {}, superseded {}, members (max {}), recordings (max {}), wish to adoption {}, asked again {}, resized {}",
			w.prepared, w.adopted, w.superseded, w.maxMembers, w.maxRecordings, Percentiles(w.wishFrames), w.askedAgain, w.resized, s.prepared, s.adopted,
			s.superseded, s.maxMembers, s.maxRecordings, Percentiles(s.wishFrames), s.askedAgain, s.resized);
		coordinator.window = {};
	}
}

#else

namespace RenderGraph::Frame::Revision
{
	void Install() {}
	void AdoptLatest() {}
	void LogStats() {}
}

#endif

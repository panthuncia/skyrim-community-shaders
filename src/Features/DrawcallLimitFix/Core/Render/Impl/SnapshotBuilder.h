#pragma once

// IndirectDraws::Impl's builds ahead and snapshot builder state. Included by Impl.h, before the struct.

#include "RenderGraph/Frame/Starvation.h"

namespace DCLF
{
	namespace Direct
	{
		struct ViewBatchSet;
	}
	namespace Draws
	{
		class ViewBatchBuilder;
	}

	struct ImplSnapshotBuilder
	{
		/** @brief A publication's draws, built ahead by the snapshot builder and committed by the frames that install it. */
		struct DrawPublication
		{
			std::shared_ptr<const SceneTables> tables;
			std::shared_ptr<const StreamViews> streams;
			std::array<std::shared_ptr<MainPayload>, 2> payloads;  // kAsyncColour, kAsyncZPrepass
			std::shared_ptr<ShadowPayload> shadow;                 // with its exclusions
		};
		/** @brief The main payload a publication's payload takes its parts from. The colour build's if present, else the Z-prepass's. */
		static const MainPayload* NewestMainPayload(const DrawPublication& a_draws)
		{
			return a_draws.payloads[kAsyncColour] ? a_draws.payloads[kAsyncColour].get() : a_draws.payloads[kAsyncZPrepass].get();
		}
		struct AheadContext
		{
			bool valid = false;
			std::array<MainInputs, 2> inputs;
			std::array<bool, 2> build{};
			std::shared_ptr<Resources> target;
			bool shadow = false;
			ShadowInputs shadowInputs;
			std::shared_ptr<ShadowResources> shadowTarget;
			// Paired with a publication committed under other toggles, the shadow part is not that publication's and is dropped.
			ToggleSet toggles{};
		};
		// Render thread. The fits the builds ahead are made against, main and shadow.
		std::array<SceneFit, 2> postedFit{};
		std::uint64_t postedFitSerial = 0;
		// A fit reads only the adopted set and the main and shadow resources, so it is remade only when one of them changes.
		std::uint64_t fitSetSerial = ~0ull;
		std::weak_ptr<const void> fitMain, fitShadow;
		/** @brief The frame start's immutable post for the coordinator, latest wins. The coordinator reads only its copy. */
		struct AheadPost
		{
			AheadContext context;
			std::array<SceneFit, 2> fit{};
			std::uint64_t fitSerial = 0;
		};
		::RenderGraph::Frame::LatestSlot<AheadPost> aheadSlot;
		AheadPost aheadTaken;  // the coordinator's
		// Context for main resources made at a frame's Setup, so a snapshot for new resources never waits for a coordinator publication.
		::RenderGraph::Frame::LatestSlot<AheadContext> resourcesAheadSlot;
		AheadContext resourcesAhead;  // the builder's
		std::vector<std::shared_ptr<MainPayload>> payloadPool;  // the snapshot builder's
		// Builds ahead posted by the coordinator and done by the builder. Fallback streams are refused while one is pending.
		std::atomic<std::uint64_t> aheadKicked{ 0 };
		std::atomic<std::uint64_t> aheadDone{ 0 };
		/** @brief The coordinator's hand-off to the snapshot builder, latest wins. */
		struct SnapshotWork
		{
			std::shared_ptr<const ScenePublication> publication;
			AheadContext context;
			std::uint32_t frame = 0;
			std::shared_ptr<const EntrySpace> sun, light;
			std::uint64_t aheadNumber = 0;  // aheadKicked when posted
		};
		/** @brief A complete scene snapshot handed from the builder to the frame. */
		struct SceneSnapshot
		{
			std::uint64_t serial = 0;
			std::shared_ptr<const ScenePublication> publication;
			std::shared_ptr<const DrawPublication> draws;
			// Null without draws or when a version's build failed. A snapshot without one draws nothing.
			std::shared_ptr<const ScenePayload> payload;
			org::async::RevisionAssembler::Lease revision;
			// Both main payloads were built for the main resources. Without them its frames have no claims.
			bool drawsComplete = false;
			// Its toggles are its publication's, which become the frame's once installed, so they are never stale.
			std::uint64_t buildGeneration = 0, mainGeneration = 0;
			// The build each segment's recordings are of: its attached or its staged one when the inputs were posted.
			std::array<std::uint64_t, static_cast<std::size_t>(RenderGraphRuntime::Segment::Count)> epochBuilds{};
			// What its graphs are built from. It becomes the attached build state when the snapshot is adopted.
			std::shared_ptr<const Draws::BuildState> build;
			// The frame versions its recordings named (Frame::Revision::FragmentVersions::serial). The frame revision adopts it alone only
			// while the frame runs those, and with the revision of that serial while it is in preparation.
			std::uint64_t frameVersions = RenderGraph::Frame::Revision::kAnyFrameVersions;
			// CPU pipeline view batches, made only while the graph is the CPU pipeline's. Null otherwise or on failure.
			std::shared_ptr<const Direct::ViewBatchSet> viewBatches;
		};
		/** @brief One snapshot build's times. Draws are 0 when kept. */
		struct SnapshotTiming
		{
			double aheadMs = 0.0, shapesMs = 0.0, recordingsMs = 0.0;
			bool ahead = false;
		};
		// A snapshot whose recordings named frame versions the builder no longer knows. The frame revision never adopts one.
		static constexpr std::uint64_t kUnknownFrameVersions = UINT64_MAX - 1;
		::RenderGraph::Frame::LatestSlot<SnapshotWork> snapshotWorkSlot;  // the coordinator -> the builder
		// The builder posts its snapshots to the frame revision (Frame::Revision::PublishFragment), which adopts them.
		// Publication pacing. The builder stores with release and the coordinator loads with acquire.
		std::atomic<std::uint64_t> snapshotWorkTaken{ 0 };
		// Gate for publishing a payload only for a snapshot the frame will adopt. The frame stores the frame first, then the serial,
		// both with release. The coordinator stores the frame of the last finished scene pass.
		std::atomic<std::uint32_t> snapshotTakenFrame{ 0 };
		std::atomic<std::uint64_t> snapshotTaken{ 0 };
		std::atomic<std::uint32_t> scenePassDoneFrame{ 0 };
		// Set when the builder waits on the gate, so the scene pass's end wakes it. Both sides use seq_cst so one always sees the
		// other and no wake is missed. Otherwise a pass end, which happens several times a frame, wakes nobody.
		std::atomic<bool> publicationGated{ false };
		std::atomic<std::uint64_t> passDoneWakesSkipped{ 0 };
		/**
		 * @brief The gate is closed while the frame has not taken the last snapshot, or that frame's scene pass has not ended. No build
		 * starts while it is closed.
		 */
		bool PublicationGateClosed() const
		{
			const std::uint64_t taken = snapshotTaken.load(std::memory_order_acquire);
			if (taken < snapshotBuilder.serial)
				return true;
			return scenePassDoneFrame.load(std::memory_order_seq_cst) < snapshotTakenFrame.load(std::memory_order_acquire);
		}
		// Whether the newest snapshot found was stale. No claims until a current one arrives.
		std::shared_ptr<const SceneSnapshot> adoptedSnapshot;
		// The snapshot also holds the installed payload, so the builder releases it. Epochs read the frame payload's parts by value.
		std::shared_ptr<const ScenePayload> installedPayload;
		PayloadVersions framePayload;
		bool framePayloadMade = false;
		/** @brief The frame's payload, or null before frame start made it. A covered epoch always has one. */
		const PayloadVersions* InstalledPayload() const { return framePayloadMade ? &framePayload : nullptr; }
		/** @brief The installed snapshot's payload, or null. */
		const ScenePayload* InstalledScenePayload() const { return installedPayload.get(); }
		/** @brief The adopted snapshot's view batches, or null in the GPU pipeline or when none were made. */
		const std::shared_ptr<const Direct::ViewBatchSet>& InstalledViewBatches() const
		{
			static const std::shared_ptr<const Direct::ViewBatchSet> kNone;
			return adoptedSnapshot ? adoptedSnapshot->viewBatches : kNone;
		}
		bool snapshotStale = false;
		// Render thread. Whether this Begin took a scene; Begins that took none are counted (snapshotFramesKept).
		bool sceneTakenThisBegin = false;
		// Scenes not adopted since the last report, by Frame::Revision::FragmentDrop, and in the session for the log's rate limit.
		std::array<std::uint64_t, 4> scenesDropped{};
		std::uint64_t scenesDroppedLogged = 0;
		// Scenes dropped for their builds since the last new build state (restageInputSerial): the one starvation rule.
		::RenderGraph::Frame::StarvationWatch restageWatch{ "DCLF's staged builds" };
		std::uint64_t restageInputSerial = 0;
		// Adopted scenes with a recording of a version the frame does not read, in the session (CheckSceneVersions).
		std::uint64_t staleSceneVersions = 0;
		// Frames whose adopted scene was sealed before an idle buffer move, in the session (SceneBufferSet::AddressesCurrent).
		std::uint64_t addressWaitFrames = 0;
		// Render thread. Whether the adopted scene binds a version the frame does not read, checked at each adoption that could change it.
		// Such a scene covers nothing.
		bool sceneVersionsStale = false;
		std::uint64_t versionsCheckedRevision = ~0ull;
		std::uint64_t versionsCheckedScene = ~0ull;  // the serial of the scene checked last
		// Frames whose coverage ran at least one DCLF epoch, in the session. The frame revision waits for DCLF's scene only while
		// this moves (Frame::Revision, plan 7.8).
		std::atomic<std::uint64_t> sceneRecordedFrames{ 0 };
		// Snapshots adopted with staged builds since the last report.
		std::uint64_t stagedAdoptions = 0;
		// Commits and latch writes take every versioned buffer's address, SRV index and capacity from this.
		const NamedVersions* adoptedSet = nullptr;
		/** @brief The adopted set. Names nothing before the first revision is adopted. */
		const NamedVersions& Adopted() const
		{
			static const NamedVersions kNone;
			return adoptedSet ? *adoptedSet : kNone;
		}
		// Replaced snapshots go back to the builder to drop, so nothing of a snapshot is released on the render thread.
		Retired<SceneSnapshot> retiredSnapshots;
		/** @brief The builder's state. One pass at a time on the pool, woken by posts, completions and growths. */
		struct SnapshotBuilder
		{
			enum class Stage : std::uint8_t
			{
				Idle,  // remade when the frame's inputs move
				Make,
				Waiting,  // none sealed because a growth or build is pending
				// Sealed and draws built. Waits for recordings and GPU-ready versions, and publishes the payload once the frame will adopt it.
				Recordings,
			};
			Stage stage = Stage::Idle;
			std::unique_ptr<SnapshotWork> work;
			// Work taken while the gate is closed. The coordinator is not told it was taken, so it publishes no more meanwhile.
			std::unique_ptr<SnapshotWork> held;
			std::shared_ptr<const DrawPublication> draws;
			// Published once per draws. A snapshot remade for the same draws keeps it.
			std::shared_ptr<const ScenePayload> payload;
			// The source whose versions are in flight. When a payload fails, its snapshot is posted without one and the next make rebuilds it.
			std::shared_ptr<const PayloadSource> payloadSource;
			bool payloadFailed = false;
			// Made once the payload is published, and posted with the snapshot once their sequences are GPU ready.
			std::shared_ptr<Draws::ViewBatchBuilder> viewBatches;
			std::uint64_t drawsSerial = 0;  // serial of the sealed set the draws embed addresses from, 0 for none
			std::uint64_t sealedSequence = 0;  // the assembler sequence of the revision sealed for work
			std::uint64_t serial = 0, lastPublication = 0;
			std::chrono::steady_clock::time_point sealedAt{};
			SnapshotTiming timing;
			// Report counters, reset by StoresReport.
			std::uint64_t publicationsWaited = 0, replacedUnpublished = 0;
			bool publicationWaited = false;  // the draws' publication has waited (publicationsWaited counted)
			// Report counters for ProducerReport.
			std::uint64_t passes = 0, idlePasses = 0, passedThrough = 0;
			std::uint64_t buildsStarted = 0, buildsDiscarded = 0, heldPasses = 0, stagesInline = 0, stagesPooled = 0;
			std::uint64_t contextsReplaced = 0;
			bool drawsPosted = false;  // draws were posted in a snapshot (buildsDiscarded counts those that were not)
			bool fresh = false;  // work taken and not yet made (PassThrough is tried once, for it)
			/**
			 * @brief The last snapshot posted with a revision and complete draws, and what it was built from. A publication with the same
			 * inputs would build the same snapshot, so it is posted again instead to carry that publication's deltas.
			 */
			struct Posted
			{
				std::shared_ptr<const ScenePublication> publication;
				std::shared_ptr<const EntrySpace> sun, light;
				std::shared_ptr<const DrawPublication> draws;
				std::shared_ptr<const ScenePayload> payload;
				std::shared_ptr<const Direct::ViewBatchSet> viewBatches;  // its snapshot's (posted again with it)
				org::async::RevisionAssembler::Lease revision;
				std::uint64_t drawsSerial = 0;
				bool valid = false, shadow = false;
				std::array<bool, 2> build{};
				const void* target = nullptr;
				const void* shadowTarget = nullptr;
				std::array<std::uint32_t, 2> vsMask{}, psMask{};
			} posted;
		} snapshotBuilder;
		/**
		 * @brief What the last make that sealed a revision read. A make with the same key keeps that revision.
		 */
		struct RevisionReuse
		{
			struct Key
			{
				// The tables are compared by value, never by identity. The coordinator pools them, so the same object can come back with other rows.
				// The catalog is held weakly, so a freed one is never mistaken for a new one at the same address.
				std::uint64_t tables = 0;
				std::weak_ptr<const PipelineCatalog> catalog;
				std::uint32_t tablesGeneration = 0;
				RevisionRequest request;
				std::uint64_t inputs = 0;     // RevisionInputs::generation
				ToggleSet toggles{};          // the publication's (RevisionProducer::toggles): moved with the inputs
				std::uint64_t lookups = 0;    // the lookups' entries the pass reads (RevisionMake::lookupsWitness)
				std::uint64_t shadowRows = 0; // and the shadow map rows the shapes read (ShadowRowsWitness)
				std::uint64_t volatiles = 0;  // other inputs, such as the event lists' asks
			} key;
			bool valid = false;      // key is the last sealed revision's, which stands (not failed since)
			bool reusedNow = false;  // this make kept the last revision (SnapshotPass takes it as sealed)
			std::uint64_t setSerial = 0;  // the set's serial the last seal named (the parity's)
			// Report counters for ProducerReport.
			std::uint64_t kept = 0, ran = 0;
			// Report counters for makes that ran with nothing to keep, and the others by which part of the key moved.
			enum Moved : std::size_t
			{
				kMovedTables,
				kMovedCatalog,
				kMovedGeneration,
				kMovedRequest,
				kMovedInputs,
				kMovedLookups,
				kMovedShadowRows,
				kMovedVolatiles,
				kMovedShrink,  // a shrink due or settled (SceneBufferSet::ShrinkPending, MainLayoutWindows::Due)
				kMovedParts
			};
			static constexpr std::array<const char*, kMovedParts> kMovedNames{ "tables", "catalog", "tables generation", "request", "inputs", "lookups", "shadow rows",
				"volatiles", "shrink" };
			std::uint64_t ranUnkept = 0;
			std::array<std::uint64_t, kMovedParts> movedBy{};
			/** @brief Counts a make that ran the pass. a_moved has a bit per Moved, ~0u when there was nothing to keep. */
			void NoteRan(std::uint32_t a_moved)
			{
				++ran;
				if (!valid) {
					++ranUnkept;
					return;
				}
				for (std::size_t part = 0; part < movedBy.size(); ++part)
					movedBy[part] += (a_moved >> part) & 1u;
			}
		} revisionReuse;
		std::atomic<bool> snapshotInputsMoved{ false };  // the frame posted revision inputs since the builder's last make
		/**
		 * @brief The newest work, once a build may start from it. Otherwise null, with the work held and the gate's opening armed to wake the builder.
		 */
		std::unique_ptr<SnapshotWork> WorkToBuild();
		std::atomic<bool> producerReportWanted{ false };
		::RenderGraph::Frame::LatestSlot<std::string> producerReportSlot;      // the builder's report lines, for the frame's report
		std::string producerReport;                      // the frame's: the newest taken
	};
}

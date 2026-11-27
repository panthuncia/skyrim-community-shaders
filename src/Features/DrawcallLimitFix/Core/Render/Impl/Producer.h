#pragma once

// IndirectDraws::Impl's revision producer and scene revision state. Included by Impl.h, before the struct.

#include "RenderGraph/Frame/EpochRecordings.h"

namespace DCLF
{
	struct ImplProducer
	{
		/** @brief What a revision's main shapes need from the frame's captures, per segment. */
		struct MainEpochInputs
		{
			std::array<bool, 2> known{};
			std::array<MainViewport, 2> viewport{};
			std::array<FrameBlockSizes, 2> blockSizes{};
		} mainEpochInputs;
		/** @brief The revision producer's state, owned by the snapshot builder. Its only inputs are RevisionInputs and its request. */
		struct RevisionProducer
		{
			RevisionInputs inputs;
			// The toggles of the publication being made, never the frame's toggles at the time the inputs were posted.
			ToggleSet toggles{};
			std::uint32_t frame = 0;
			RevisionLatchLayouts latches;
			RevisionLatchHandles latchHandles;
			MadeShapes made;
			std::vector<ShadowViewLayout> occlusionLayouts;
			// Recent sets of shadow views in use, newest first. Each gets its own recording variant.
			std::vector<std::uint64_t> recentActiveViews;
			DrawBoundStore drawBound;
			IndexPool::Bound poolBound;
			RowBucketsCache rowBuckets;
			MainLayout mainLayout;
			MainLayoutWindows mainWindows;
			bool mainLayoutMade = false;
			std::uint32_t shadowBucketWords = 0;
			// The graph build a frame request needs, 0 for none. Nothing is sealed before it, or the recordings would lack its declarations.
			std::uint64_t buildWanted = 0;
		} producer;
		/** @brief Brings the draw bound up to date with the tables' change log. */
		void UpdateDrawBound(const SceneTables& a_tables, std::uint32_t a_generation);
		/** @brief The newest RevisionInputs from the render thread, latest wins. Taken at each make's start. */
		::RenderGraph::Frame::LatestSlot<RevisionInputs> revisionInputsSlot;
		RevisionInputs postedInputs;  // what the render thread last posted
		/** @brief Shadow view slot requests, served at the build point before its build. */
		::RenderGraph::Frame::LatestSlot<ShadowSlotsRequest> shadowSlotsSlot;
		/**
		 * @brief The scene revisions. A recording needs exactly its shape and versions, so an unchanged epoch inherits the last recording
		 * and any other is recorded on ORG's host thread. Each member says whether it belongs to the builder, the frame or the completions.
		 */
		struct SceneRevisions
		{
			// Slot layout by epoch. An epoch that shares a shape holds its source's.
			static constexpr std::uint32_t kVersionsSlot = 0, kShapeSlot = 1, kRecordingSlot = kShapeSlot + kMaxEpochs, kSlots = kRecordingSlot + kMaxEpochs;
			// Only the builder uses it. A revision's last recording completing wakes the builder.
			org::async::RevisionAssembler assembler{ kSlots, [] { WakeSnapshotBuilder(); } };
			SealedSet sealed;
			std::shared_ptr<const MainLayout> layout;
			// A recording belongs to the graph build it was made on, so a new build needs a new fragment and every epoch is recorded again.
			std::shared_ptr<const org::async::RevisionFragment> versionsFragment;
			std::shared_ptr<const NamedVersions> versionsFragmentSet;  // the sealed set versionsFragment was made for
			std::uint64_t graphBuilds = ~0ull;  // the build versionsFragment was made for (RevisionInputs::buildGeneration)
			// The frame versions versionsFragment was made for (RevisionInputs::frameVersionsSerial). New ones make every epoch record
			// again against them.
			std::uint64_t frameVersions = ~0ull;
			std::uint64_t growthWaits = 0;  // makes whose set was pending on a growth, since the last report
			std::uint64_t buildWaits = 0;  // makes that sealed nothing while waiting for a graph build
			std::uint64_t seals = 0, versionSets = 0, sealFailures = 0;
			// What the last sealed revision names, which its snapshot's draws embed.
			std::shared_ptr<const NamedVersions> revisionVersions;
			std::shared_ptr<const MainLayout> revisionLayout;
			struct EpochStats
			{
				std::uint64_t changed = 0, requested = 0, refused = 0;
				// Written by completions on ORG's host thread.
				std::atomic<std::uint64_t> recorded = 0, failed = 0, dropped = 0, shapes = 0;
			};
			std::array<EpochStats, kMaxEpochs> epochs;
			std::atomic<std::uint32_t> failuresLogged = 0;
			// The frame owns everything from here on.
			org::async::RevisionAssembler::Lease active;
			std::uint64_t latchedMisses = 0;
			// Per epoch since the last report. variants counts submissions of a recording other than the first.
			std::array<std::uint64_t, kMaxEpochs> unrevised{}, latchClamped{}, submitted{}, variants{};
			std::uint64_t occlusionSubmitted = 0;
			// Per epoch, whether the selected revision has recordings for the graph as built now.
			std::array<bool, kMaxEpochs> covered{};
			// Per epoch, why the engine draws it when it is not covered (DecideEpochCoverage).
			std::array<EngineState, kMaxEpochs> engine{};
			bool explicitBuilds = false;
			// Uncovered frames happen only while DCLF does not draw or under a load screen.
			std::uint64_t builds = 0, uncoveredFrames = 0, ticketsReleased = 0;
			// The first uncovered frame since the last report and its CoverageState, for the report's line. Empty when none was.
			std::string firstUncovered;
			std::array<std::uint64_t, kMaxEpochs> uncovered{};
			/** @brief What an epoch's recordings are for: exactly these fragments (the versions, the shape, then each hosted shape). */
			struct RecordingInputs
			{
				std::vector<std::shared_ptr<const org::async::RevisionFragment>> fragments;
				friend bool operator==(const RecordingInputs&, const RecordingInputs&) = default;
			};
			/** @brief Hashes an epoch's recording inputs by the identity of each fragment. */
			struct RecordingInputsHash
			{
				std::uint64_t operator()(const RecordingInputs& a_inputs) const
				{
					std::uint64_t hash = 0xcbf29ce484222325ull;
					for (const auto& fragment : a_inputs.fragments)
						hash = (hash ^ static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fragment.get()))) * 0x100000001b3ull;
					return hash;
				}
			};
			// The current inputs and the last ones. A drop is not asked for again until the inputs change.
			static constexpr std::size_t kRecordingsKept = 2;
			// Per epoch. An entry's payload is the fragment its recordings resolve. The builder looks up and adds.
			using RecordingCache = ::RenderGraph::Frame::EpochRecordings::Cache<RecordingInputs, kRecordingsKept,
				::RenderGraph::Frame::EpochRecordings::AfterDrop::KeepUntilNewKey, RecordingInputsHash, std::shared_ptr<org::async::RevisionFragment>>;
			std::array<RecordingCache, kMaxEpochs> recordingCaches;
		} revisions;
		/** @brief Recording requests not yet completed. The build point releases unsubmitted tickets for them. */
		std::atomic<std::uint32_t> recordingsOutstanding{ 0 };
		/** @brief Groups of what the shapes are made from, hashed separately. Shapes are remade only when a group moves. */
		enum ShapesKeyGroup : std::size_t
		{
			kKeyVersions,
			kKeyPipelines,
			kKeyLookups,
			kKeyMain,
			kKeyShadow,
			kKeyBound,
			kKeyReflection,
			kKeyFirstPerson,
			kKeyGroups
		};
		static constexpr std::array<const char*, kKeyGroups> kKeyGroupNames = { "named set", "pipelines", "lookups", "main latch and captures", "shadow latch and layouts",
			"casting bound outgrown", "reflection", "first person" };
		using ShapesKey = std::array<std::uint64_t, kKeyGroups>;
		ShapesKey revisionShapesKey{};
		std::array<std::uint64_t, kKeyGroups> shapesMadeBy{};  // since the last report, by the group whose inputs moved
		std::uint64_t shapesMade = 0, shapesKept = 0;
		// A move of the casting bound alone keeps the shapes. Only outgrowing it remakes them.
		std::uint64_t shadowBoundChecked = ~0ull, shadowBoundOutgrown = 0;
	};
}

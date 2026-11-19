#pragma once

// The scene pump: the pass mode, the publication key and its pacing, and what a pass holds for the render thread.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorPump
	{
		/** @brief Set by the render thread. Unchanged passes publish nothing. */
		std::atomic<bool> passesFull{ false };
		std::uint64_t unpublishedNs = 0;
		std::uint32_t unpublishedFrame = 0;
		using PublishKey = DCLF::PublishKey;
		PublishKey publishedKey;
		bool publishedKeyValid = false;
		bool publishForced = false;  // an input TakeCoordinatorInputs took publishes
		// A pass publishes only once the snapshot builder took the last publication, unless it is the frame's own pass, inline or the first.
		// Otherwise it defers and its changes ride the next one.
		std::uint32_t passSources = 0;      // the SceneWake bits the running pass took
		bool passTookFrameInputs = false;   // the running pass took new frame inputs (TakeFrameInputs)
		bool passDeferred = false;          // the running pass needed a publication and deferred it
		std::uint32_t passesDeferred = 0;   // passes deferred since the last publication (folded into the next)
		std::uint64_t pacedPublications = 0;  // publications that folded deferred passes in (the paced parity's schedule)
		bool pacedCheckDue = false;           // this publication is the paced parity's (Policy::CheckPacedPublication)
		std::shared_ptr<SetSnapshot> claimsReference;  // its claims made whole (PublishClaims), compared after
		enum class PublishReason : std::uint8_t
		{
			First,   // nothing published yet
			Inline,  // the parities' inline pass
			Frame,   // the frame's own pass
			Taken,   // the builder took the last publication
			Count
		};
		struct PaceStats
		{
			std::uint64_t passes = 0, published = 0, deferred = 0, folded = 0;  // full passes, publications, deferred passes, of them folded
			std::array<std::uint64_t, static_cast<std::size_t>(PublishReason::Count)> byReason{};
			std::array<std::uint64_t, 5> neededBy{};
			std::array<std::uint64_t, std::tuple_size_v<decltype(PublishKey::words)> + 2> keyMoved{};
			std::array<std::uint64_t, 12> deltaPending{};
			std::array<std::uint64_t, 6> lookupsMoved{};
			std::uint32_t frames = 0, lastFrame = ~0u, passesThisFrame = 0, maxPasses = 0, publishedThisFrame = 0, maxPublished = 0;
			std::uint64_t claimsReplayed = 0, claimsKeys = 0, claimsWhole = 0, claimsShared = 0;
			double restMs = 0.0;  // PublishTables' whole-copied rest, per publication summed
			std::uint64_t replayLogEntries = 0, replayMaterialEntries = 0, replaySlots = 0, replayMaterials = 0, replayColumns = 0, replaySkipped = 0, replayRuns = 0,
						  replayPooled = 0, replayNearer = 0;
			double replayMs = 0.0;
			std::uint64_t forwardCounted = 0, forwardKept = 0, waitingCompacted = 0, waitingKept = 0;
			std::uint64_t parityChecks = 0, parityTables = 0, parityClaims = 0;
			std::string parityFirst;
		} paceStats;
		std::uint32_t paceWindow = ~0u;
		struct PumpStats
		{
			std::vector<double> passMs;                // each pass's whole time
			std::vector<std::uint32_t> passEvents;     // tracker events and queue entries drained
			std::vector<double> publishMs;             // oldest unpublished event to its publication
			std::vector<std::uint32_t> publishFrames;  // the same in render-thread frames
			std::uint64_t passes = 0, eventsOnly = 0, loading = 0, unchanged = 0, published = 0, inlinePasses = 0, failed = 0;
			std::array<std::uint64_t, static_cast<std::size_t>(SceneWake::Count) + 1> bySource{}, unchangedBySource{};
			std::uint64_t perFrameWalks = 0, eventWalks = 0;
			double fullMs = 0.0, fullMaxMs = 0.0;  // whole passes (not events alone)
			std::uint32_t firstFrame = 0, lastFrame = 0, framesWithPass = 0, passesThisFrame = 0, maxPassesInFrame = 0;
			bool any = false;
		} pumpStats;
		bool holdLostMembers = false;  // from the joins on: members RunSceneWork drops are held for PrimaryCull
		// What a scene pass holds for the render thread while it runs.
		bool inSceneTask = false;       // on the lane, not inline
		bool holdPrimaryNotes = false;  // PrimaryCull's notes held (primaryNotes)
	};
}

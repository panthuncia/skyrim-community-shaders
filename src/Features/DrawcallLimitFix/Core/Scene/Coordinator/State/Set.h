#pragma once

// The DCLF set and the claims: the waiting slots and the commit queue, the commit's decision and application, readiness stamps
// and the claims pool.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorSet
	{
		LogCursor setCommitCursor;
		// Bound slots waiting for readiness, read again when a readiness source moved.
		std::vector<std::uint32_t> setWaiting;
		std::vector<std::uint8_t> setWaitingMark;  // per slot: in setWaiting
		std::vector<std::uint32_t> setQueue;
		std::vector<std::uint8_t> setQueueMark;    // per slot: in setQueue
		std::vector<std::uint8_t> setLacking;      // per slot: occluder and reflection phases it takes part in but is not a member of
		std::array<std::uint32_t, 3> setLackingCount{};  // per occlusion map, then the reflection
		std::vector<std::uint8_t> setPartialMark;  // per slot: out of the set with some phases ready
		std::uint32_t setPartialCount = 0;
		// The last commit's decision for the next ApplySet. A slot holding another geometry by then takes nothing.
		std::vector<std::uint8_t> setPhasesNext, setLackingNext;
		std::vector<std::pair<std::uint32_t, const RE::BSGeometry*>> setApply;
		std::vector<const RE::BSGeometry*> setGeometryNext;
		std::vector<std::uint32_t> setApplyMark;  // per slot: its index in setApply + 1, 0 when absent
		std::uint32_t setCommitFrame = 0;
		std::uint32_t setCommitToggles = 0;  // the toggles generation the last commit was made under
		ToggleSet setCommitToggleSet{};      // ... and the set (the frame inputs', which the pass evaluated under)
		std::vector<std::uint8_t> setPhasesApplied;
		std::vector<const RE::BSGeometry*> setGeometryApplied;
		LogCursor claimCheckCursor;
		// Claims a publication made that its tables do not draw. A defect, never handed back to the engine.
		std::uint64_t undrawnClaims = 0;
		std::string undrawnFirst;
		std::vector<const RE::BSGeometry*> setGeometry;  // per slot: the geometry a base member is published under
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> setMemberSlot;  // published geometry -> its slot
		LogCursor setCursor;
		static constexpr std::uint32_t kWaitSources = 4;
		static constexpr std::uint8_t kWaitLookups = 1, kWaitShadow = 2, kWaitConstants = 4, kWaitFit = 8;
		static constexpr std::uint8_t kWaitOther = 16;  // any source moving retakes it (geometry, decal slot, reflection, layer partner)
		std::array<std::uint64_t, kWaitSources> setReadiness{ ~0ull, ~0ull, ~0ull, ~0ull };
		// No readiness source moves with the forward views' pipelines, so a count change retakes the kWaitOther slots.
		std::uint64_t setForwardReadiness = ~0ull;
		// Held so no other catalog can take its address.
		std::shared_ptr<const PipelineCatalog> setForwardCatalog;
		std::uint64_t setForwardCount = 0;
		std::vector<std::uint8_t> setWaitCause;  // per slot: what it waits on
		std::vector<std::uint8_t> setWaitWhy;    // per slot: the first reason of its last wait
		std::uint64_t setShadowModes = ~0ull;      // the shadow modes and states caster readiness was last taken under
		std::uint32_t setPhaseMask = ~0u;          // the phases DCLF draws at the last commit
		std::shared_ptr<SetSnapshot> setBuilding;  // the next publication, kept current by each commit
		EventLog<const RE::BSGeometry*> setPhaseLog;
		// Published claims are pooled like the tables. A free entry is brought up to date by the phase log, or copied whole if the log no longer reaches it.
		struct ClaimsEntry
		{
			std::shared_ptr<SetSnapshot> snapshot;
			LogCursor cursor;
			std::vector<const RE::BSGeometry*> edited;  // keys the revocations changed: not setBuilding's
		};
		std::vector<ClaimsEntry> claimsPool;
		std::size_t claimsPublished = ~std::size_t(0);  // publicationClaims' entry
		bool claimsFresh = false;                       // made by this publication (else shared from the last: held, never written)
		bool setSnapshotDirty = true;  // setBuilding changed since the last published claims
		using PendingMap = ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint8_t>;
		PendingMap setPending;
		bool setPendingDirty = true;
		// A face shape waits pending for its head's snapshot, so the engine does not draw the head between two snapshots.
		// After kStreamWaitFrames it is the engine's again.
		static constexpr std::uint32_t kStreamWaitFrames = 120;
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> streamWaitingSince;
		bool setPendingChanged = false;
		struct SlotDecision
		{
			std::uint8_t phases = 0;  // every phase it takes part in if ready for all, else 0
			bool wait = false;        // takes part in a phase it is not ready for
			bool partial = false;
			std::array<std::uint8_t, 8> whys{};  // waiting reasons in order (waitingBy's index)
			std::uint8_t whyCount = 0;
			bool rootViewExcluded = false;       // observed runs: a CategoryRoot view's slot its view takes no part in
			bool rootViewHeld = false;           // a CategoryRoot view's slot held out of its view
			std::uint8_t heldWhyCount = 0;       // the reasons noted before it was held out
			std::uint32_t rootViewSlotWhy = 0;   // IndirectDraws::ViewSlotReady's cause
		};
	};
}

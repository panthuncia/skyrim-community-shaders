#pragma once

// Placements, moves and root motion: the walk's placement plans and the planned set they diff against, the mover set, moving and
// still roots, and move events.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorPlacements
	{
		std::shared_ptr<PlacementPlan> placementPlan, placementPlanReady;
		// The next publication carries every plan no publication took yet.
		std::vector<std::shared_ptr<PlacementPlan>> placementPlansHeld;
		/**
		 * @brief The placement set the plans carry. Keys only, since a held reference dropped by a later pass would be released on the lane.
		 * References are copied from the entries and root owners when a plan is made.
		 */
		ankerl::unordered_dense::set<RE::BSGeometry*> moverSet;
		ankerl::unordered_dense::set<const RE::NiAVObject*> stillRoots;
		// The next plan must be a reset.
		bool placementDirty = true;
		using PlannedKey = PlacementPlan::Key;
		/** @brief The set as the plans so far left it, matching FrameValues' kept set. A plan costs what changed, never the set. */
		ankerl::unordered_dense::map<const void*, PlannedKey> plannedMovers;                                                   // by geometry
		ankerl::unordered_dense::map<const RE::NiAVObject*, ankerl::unordered_dense::map<const void*, PlannedKey>> plannedRoots;  // by node: dependents
		struct PlannedOwner
		{
			const void* geometry = nullptr;
			const RE::NiAVObject* root = nullptr;
			bool operator==(const PlannedOwner&) const = default;
		};
		ankerl::unordered_dense::map<std::uint32_t, std::vector<PlannedOwner>> plannedSlots;
		std::uint32_t plannedObjectSlots = 0, plannedBoneCapacity = 0, plannedObjectStamp = 0;
		LogCursor placementCursor;  // the change log, read for the planned slots
		// Keys that may differ from the planned set since the last plan. Duplicates compare equal the second time.
		std::vector<RE::BSGeometry*> placementMoversTouched;
		std::vector<const RE::NiAVObject*> placementRootsTouched;
		std::vector<std::pair<const RE::NiAVObject*, const void*>> placementDependentsTouched;
		struct PlacementDeltaStats
		{
			std::uint64_t plans = 0, resets = 0, sized = 0;
			std::uint64_t moversJoined = 0, moversChanged = 0, moversLeft = 0, rootsListed = 0, rootsLeft = 0, dependentsJoined = 0, dependentsChanged = 0,
						  dependentsLeft = 0;
			std::uint64_t moversTouched = 0, rootsTouched = 0, dependentsTouched = 0, logSlots = 0;
			std::uint64_t parityChecks = 0, parityDiffer = 0;
			std::string parityFirst;
			std::uint64_t dirtyRoots = 0, dirtyRootsWhole = 0, rootDependentsScheduled = 0, rootDependentsListed = 0;
			std::uint64_t rootWitnessChecks = 0, rootWitnessDiffer = 0, rootWitnessSkipped = 0;  // skipped: scheduled already, or no placement matters
			std::string rootWitnessFirst;
			std::uint64_t scans = 0, scansWhole = 0, staleSlots = 0, scanChecked = 0, scanObjects = 0, scanScheduled = 0;
			std::uint64_t scanParityChecks = 0, scanParityDiffer = 0;
		} placementDeltaStats;
		std::uint32_t placementDeltaWindow = 0;
		// Keys only.
		ankerl::unordered_dense::set<const void*> mirrorMotionNodes, mirrorSkinGeometries;
		struct MoverGateStats
		{
			std::uint64_t perFrameWalks = 0, ungatedWalks = 0, due = 0, moved = 0, owed = 0, firstPerson = 0, fullEveryFrame = 0, skipped = 0;
			std::uint64_t plans = 0, plansWritten = 0, plansChanged = 0, candidatesSame = 0, perFrameWithoutPlan = 0;
			std::uint64_t mirrorNodes = 0, mirrorGeometries = 0;
			std::uint64_t witnessChecks = 0, witnessDiffer = 0;
			std::string firstDiffer;
		} moverGateStats;
		std::uint32_t moverGateWindow = 0;
		/** @brief A root whose bound is an entry's only motion is listed in the plan instead of the entry. */
		struct MovingRoot
		{
			const void* key = nullptr;  // its reference
			const RE::NiNode* categoryNode = nullptr;
		};
		ankerl::unordered_dense::map<const RE::NiAVObject*, MovingRoot> movingRoots;
		std::size_t movedKeysQueued = 0;
		struct PlacementStats
		{
			std::uint64_t roots = 0, stillRoots = 0, rootsGated = 0;
			std::uint64_t fadeRootsUnmirrored = 0;  // listed with no mirror record (FadeRootFromMirror)
		} placementStats;
		std::array<const RE::NiNode*, Extensions::kEngineViews> plannedViewRoots{};  // viewRoots as the planned keys took them (a change makes the set anew)
		ankerl::unordered_dense::map<const void*, std::uint32_t> movedFrame;
		// QueueRoots lists these references' roots whatever RootMoves says, since cell updates recompute a still root's bound.
		std::array<std::vector<const void*>, 2> movedKeys;
		std::uint32_t movedKeysFrame = 0, movedPruneFrame = 0;
		bool moveGating = false;
		std::uint32_t moveUngatedThrough = 0;
		ankerl::unordered_dense::map<const RE::NiAVObject*, bool> rootMotion;
		// Their merge must change nothing.
		ankerl::unordered_dense::set<const RE::BSGeometry*> rootWitness;
	};
}

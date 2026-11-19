#pragma once

// The publication: the pooled table snapshots and their replay, and the publication's sequence, commit and claims.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorPublication
	{
		std::vector<std::shared_ptr<const void>> retiredImports;  // into the next publication's deltas (TakeDeltas)
		// Pooled. One no publication holds any more is written again.
		std::vector<std::shared_ptr<SceneTables>> tablesPool;
		// Whether each pool entry can be brought up to date by replay. A new snapshot is copied whole once.
		std::vector<std::uint8_t> tablesPoolReplayable;
		// A replay's scratch, collected once and read by its column jobs.
		std::vector<std::uint32_t> replaySlots;
		std::vector<std::uint32_t> replaySlotCauses;
		std::vector<std::uint32_t> replayMaterialSlots;
		std::vector<std::uint8_t> replayMaterialMarks, replayGeometryMarks;
		// Smaller replays run on the lane.
		static constexpr std::size_t kReplayMinSlots = 96, kReplayMinMaterials = 24;
		// Each material record is about 2.4 KB.
		static constexpr std::size_t kReplayMaterialGrain = 32;
		std::shared_ptr<SceneTables> publishedTables;
		std::shared_ptr<const void> publishedRetirement;
		using PublicationDeltas = DCLF::PublicationDeltas;
		std::uint64_t publicationSequence = 0;
		std::uint32_t publicationCommitFrame = 0;    // the commit the next publication applies
		std::uint32_t publicationCommitToggles = 0;  // the toggles generation that commit was made under
		ToggleSet publicationCommitToggleSet{};      // ... and the set (ScenePublication::toggles)
		std::shared_ptr<const SetSnapshot> publicationClaims;
		std::vector<const RE::BSGeometry*> publicationJoined, publicationLeft;
	};
}

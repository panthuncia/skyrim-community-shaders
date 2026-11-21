#pragma once

// Attach and detach: AddSubtree's prepares and merges, their fan-out and the attach burst's counters.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorAttach
	{
		enum class SubtreeSource : std::uint8_t
		{
			AttachEvent,
			CategoryWalk,
			Retry,
		};
		std::vector<RE::NiPointer<RE::NiAVObject>> pendingSubtrees;
		std::vector<RE::BSGeometry*> validationSuspects;  // ValidateSlice's: keys, checked again only while tracked
		std::uint64_t subtreesPended = 0, subtreesDropped = 0;  // since the last report: waiting for the mirror, and still without a chain after
		std::uint64_t categoryChildrenMissing = 0;  // a new category node the capture listed no children of (should not happen)
		// A prepare is pure over the batch's frozen mirror and runs on any thread. The merge runs in event order.
		/** @brief Pure, since the mirror and batchPins are frozen while a batch's adds prepare. */
		struct HeldKeys
		{
			const void* property = nullptr;
			const void* layer = nullptr;
			const void* fadeNode = nullptr;
			RE::NiRefObject* propertyPin = nullptr;
			RE::NiRefObject* layerPin = nullptr;
			RE::NiRefObject* fadeNodePin = nullptr;
		};
		/** @brief Everything AddGeometry reads for one geometry, so the merge only stores. */
		struct GeometryPrep
		{
			RE::BSGeometry* geometry = nullptr;
			Ineligible reason = Ineligible::None;  // what the nodes between the category node and it make of it (MirrorSubtree's)
			const RE::TESObjectREFR* ownerReference = nullptr;  // its owner class's reference (Extensions::OwnerClassOf), or null
			const void* ancestorKey = nullptr;   // AncestorKeyOf
			const RE::NiNode* source = nullptr;  // its entry class's held parent (EntryRule::holdsParent: a face shape's head)
			bool mirrored = false;
			bool ranged = false;
			bool resolved = false;
			const void* sunEntry = nullptr;
			const RE::NiAVObject* lightActorRoot = nullptr;  // the category node's child above it whose reference has an owner class
			RE::NiRefObject* geometryPin = nullptr;
			RE::NiRefObject* sourcePin = nullptr;
			HeldKeys held;
			const RE::TESObjectREFR* sunEntryReference = nullptr;
			bool sunEntryCategory = false;
			Api::Mark mark;
			std::vector<Api::EntryRange> ranges;
		};
		struct SubtreePrep
		{
			RE::NiAVObject* root = nullptr;
			SubtreeSource source = SubtreeSource::AttachEvent;
			RE::NiNode* category = nullptr;
			bool unmirrored = false;  // FindCategoryNode found a record missing on the way up
			std::vector<GeometryPrep> geometries;
		};
		// A subtree's prepare walks one root to hundreds of geometries, so chunks are a few roots.
		static constexpr std::size_t kSubtreeGrain = 4;
		static constexpr std::size_t kSubtreeFanoutMin = 16;
		// Geometry chunks let a few large attaches use the pool even below kSubtreeFanoutMin roots.
		static constexpr std::size_t kGeometryGrain = 32;
		static constexpr std::size_t kGeometryFanoutMin = 64;
		std::vector<SubtreePrep> subtreePreps;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> subtreeGeometryIndex;  // PrepareSubtrees' scratch: (prep, geometry) flattened
		struct AttachStats
		{
			std::uint64_t mirrorBatches = 0, mirrorCaptures = 0, mirrorPooled = 0, mirrorRecords = 0;
			double mirrorPrepareMs = 0.0, mirrorApplyMs = 0.0;
			std::uint64_t geometryRounds = 0, geometryPooled = 0, geometries = 0, geometriesPooled = 0;
			std::uint64_t categoriesLeft = 0, categoryMembersDropped = 0, rootsLeftScanned = 0;
			static constexpr std::size_t kMirrorKinds = 6;
			std::array<std::uint64_t, kMirrorKinds> mirrorKindEvents{};
			std::array<double, kMirrorKinds> mirrorKindMs{};
			std::uint64_t replaysSkipped = 0, replaysWalked = 0, updatesKept = 0, updatesForgotten = 0;
			std::uint64_t structuralEvents = 0, detachedGeometries = 0, detachedErased = 0, leafKeys = 0, leafHolds = 0;
			double ancestorMs = 0.0, detachMs = 0.0, mergeMs = 0.0, leafMs = 0.0;
		} attachStats;
	};
}

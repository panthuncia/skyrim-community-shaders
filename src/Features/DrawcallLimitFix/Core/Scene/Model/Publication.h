#pragma once

// What crosses from the coordinator to the render thread with a publication: the immutable ScenePublication, and the
// publication log's per-publication deltas. Made on the scene lane, read on the render thread once installed.

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Lookups.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Scene/EngineCasters.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF
{
	struct PipelineCatalog;
	struct EntrySpace;

	/** @brief The counts a scene revision sizes by besides its publication's own tables, lookups and catalog. */
	struct RevisionRequest
	{
		std::uint32_t pipelines = 0;
		std::size_t shadowSlotKeys = 0, shadowKeys = 0;
	};

	/**
	 * @brief The newest tables and lookups, for a reader off the frame's path such as texture streaming. It is not the publication,
	 * so the retirement chain stays on the frame's path. The pieces' last releases are safe on any thread.
	 */
	struct PublishedScene
	{
		std::shared_ptr<const SceneTables> tables;
		std::shared_ptr<const Lookups> lookups;
	};
	PublishedScene LatestPublishedScene();

	/**
	 * @brief Why a bound object waits to join the set, with a name for the logs. kSetTransientWaits marks the reasons a load's objects
	 * settle from by themselves, which is what a load's hold waits on.
	 */
	inline constexpr std::size_t kSetWaitReasons = 14;
	inline constexpr std::array<const char*, kSetWaitReasons> kSetWaitReasonNames{ "pipeline", "material", "shadow mask", "shared lookups", "geometry",
		"decal slot", "layer partner", "shadow pipelines", "reflection pipeline", "constants", "scene buffers' growth", "refraction pipeline",
		"past the scene buffers (shadow)", "shadow diffuse" };
	inline constexpr std::uint32_t kSetTransientWaits = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 7) | (1u << 9) | (1u << 10) |
	                                                    (1u << 12) | (1u << 13);

	/** @brief Everything a frame reads of the scene, complete and immutable. The render thread adopts the newest one whole. */
	struct ScenePublication
	{
		std::uint64_t sequence = 0;
		std::uint32_t commitFrame = 0;        // the scene frame of the commit it applies (CommitSet's; stats and the install delay)
		std::uint32_t togglesGeneration = 0;  // the toggles generation that commit was made under (FrameInputs)
		// The frames use this toggle set while the publication is installed.
		ToggleSet toggles{};
		std::uint32_t tablesGeneration = 0;  // the tables' generation (GetTablesGeneration while it is the frame's)
		std::shared_ptr<const SceneTables> tables;
		// Slots and imports retired after this node are freed once no publication up to it lives. Only the frame's path holds it,
		// so its holders are bounded by the frames in flight.
		std::shared_ptr<const void> retirementHold;
		std::shared_ptr<const SetSnapshot> claims;
		std::array<std::uint32_t, 3> lackingCount{};
		// Waiting slots by the first reason of their wait, read by a load's hold.
		std::array<std::uint32_t, kSetWaitReasons> waiting{};
		bool built = false;
		// The pass built the scene and no slot waits on a transient reason. The render thread adds what only it sees.
		bool settled = false;

		std::uint32_t TransientWaiting() const
		{
			std::uint32_t count = 0;
			for (std::size_t why = 0; why < kSetWaitReasons; ++why)
				count += (kSetTransientWaits >> why) & 1u ? waiting[why] : 0u;
			return count;
		}
		// The lookups the draws were built with and the catalog they were resolved from.
		std::shared_ptr<const Lookups> lookups;
		std::shared_ptr<const PipelineCatalog> catalog;
		std::shared_ptr<const EntrySpace> sunCandidates, lightCandidates;
		std::uint64_t lightEntriesAppeared = 0;
		// Shadow casters only the engine's cull draws, by root. Null for none.
		std::shared_ptr<const Scene::EngineOnlySubset> engineCasters;
		// Copied only when a cell attaches or detaches. The light entries change with every tracked root, so they travel as log changes instead.
		std::shared_ptr<const ankerl::unordered_dense::set<RE::NiNode*>> categoryNodes;
		RevisionRequest revision;
		// The oldest event this is the first publication of, for event-to-adoption latency. 0 for none.
		std::uint64_t eventNs = 0;
		std::uint32_t eventFrame = 0;
	};

	/**
	 * @brief What FrameValues samples: per-frame movers, the sun entries of roots the walk took, and the slots it wrote in full.
	 * The tables hold no placement or palette, so these are the only rows the frames draw with. References are held and dropped on the render thread.
	 *
	 * A plan is a delta of the placement set. FrameValues applies every plan in order, so none may be skipped.
	 */
	struct PlacementPlan
	{
		struct Item
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			RE::NiPointer<RE::NiAVObject> listedRoot;  // its listed root, the sun entry (ResolveSunEntry); none: the entry is unbounded
			std::uint32_t slot = kNoObjectSlot;
			std::uint32_t layerSlot = kNoObjectSlot;  // its layer's row (WriteLayer): the same placement, its layer property's fade node
			// A skin's palette block, 0 rows without one.
			std::uint32_t boneOffset = 0, boneRows = 0;
			// The skin partition the mirror holds, never dereferenced. RaceMenu's body morphs store a new partition straight into the skin
			// instance with no hook, so the producer compares against this and captures the geometry again when they differ.
			const void* skinPartition = nullptr;
			// The engine moves the first-person skeleton's translations into its root's space on the render thread (FUN_1414cb020), so the
			// producer never samples it. The render thread takes its row at frame start, before the shift.
			bool firstPerson = false;
			// A palette rows class's block is filled by the class's registration every frame, as for a particle system's quads.
			Api::FrameValueItemClass itemClass = Api::FrameValueItemClass::None;
		};
		/** @brief An item as compared. Raw pointers, never dereferenced. */
		struct Key
		{
			const void* object = nullptr;
			std::uint32_t slot = kNoObjectSlot, layerSlot = kNoObjectSlot, boneOffset = 0, boneRows = 0;
			const void* sunEntry = nullptr;
			bool firstPerson = false;
			Api::FrameValueItemClass itemClass = Api::FrameValueItemClass::None;
			const void* skinPartition = nullptr;  // a new one plans the item again, so the producer compares against it
			bool operator==(const Key&) const = default;
			static Key Of(const Item& a_item)
			{
				return { a_item.geometry.get(), a_item.slot, a_item.layerSlot, a_item.boneOffset, a_item.boneRows, a_item.listedRoot.get(), a_item.firstPerson,
					a_item.itemClass, a_item.skinPartition };
			}
		};
		struct Root
		{
			RE::NiPointer<RE::NiAVObject> root;
			std::vector<Item> dependents;  // joined or changed (by geometry); their sun entry is the root's bound (listedRoot unused)
		};
		/** @brief Parity only. The coordinator's full rebuild of the set after this plan, sorted. */
		struct Check
		{
			std::vector<Key> movers;
			std::vector<std::pair<const void*, std::vector<Key>>> roots;
		};
		// Applied in this order: what left, then what joined or changed.
		std::vector<const void*> moversLeft;                             // by geometry
		std::vector<const void*> rootsLeft;                              // by node, with every dependent
		std::vector<std::pair<const void*, const void*>> dependentsLeft;  // (root node, geometry) of a root that stays
		std::vector<Item> movers;                                        // joined or changed (by geometry)
		std::vector<Root> roots;                                         // listed, or whose dependents joined or changed (by node)
		bool reset = false;                                              // the kept set is cleared first (the plan carries it whole)
		std::vector<Item> written;  // once, by the frame that takes the plan
		std::uint32_t slots = 0;         // the object slots the tables held
		std::uint32_t boneCapacity = 0;  // the palette rows their blocks span (SceneTables::BoneCapacity)
		std::uint64_t walk = 0;          // the walk that made it
		std::shared_ptr<const Check> check;  // parity only (the set after this plan, rebuilt whole): FrameValues compares its kept set
	};

	/** @brief A slot whose shading FrameValues samples at the next frame start. Holds its property so the sample never reads a freed one. */
	struct ShadingItem
	{
		RE::NiPointer<RE::BSShaderProperty> property;  // a BSLightingShaderProperty (the layer's for a layer's slot), or an effect property
		std::uint32_t slot = kNoObjectSlot;
		std::uint32_t pass = 0;   // its pipeline's pass descriptor
		bool member = false;      // a resident record's (SampleShading's a_member)
	};

	/** @brief A fade root or component node whose seed FrameValues samples in the frame that first draws its generation. Holds the node. */
	struct FadeSeedItem
	{
		RE::NiPointer<RE::NiAVObject> node;
		std::uint32_t root = kNoFadeRoot;
		std::uint32_t generation = 0;
		std::uint32_t row = 0;
	};

	using ShadingFieldValue = Api::ShadingFieldValue;

	// Light entry changes rather than copies of the set, since the set changes with every root tracked and every frame in motion.
	struct LightEntryChange
	{
		const RE::NiAVObject* node = nullptr;
		std::int8_t kind = 0;  // 1 added, -1 removed, 0 every entry cleared
	};

	/**
	 * @brief What one publication's passes made for the frame, carried by the publication log in order. Claims changes are taken up
	 * to the installed publication, frame inputs up to the newest one taken.
	 */
	struct PublicationDeltas
	{
		std::uint64_t publication = 0;  // its publication's sequence
		std::uint32_t commitFrame = 0;  // that publication's commit (the install delay)
		std::uint64_t eventNs = 0;
		std::uint32_t eventFrame = 0;
		std::vector<const RE::BSGeometry*> joined, left;
		std::vector<std::shared_ptr<const PlacementPlan>> plans;
		std::vector<ShadingItem> shading;
		std::vector<std::vector<FadeSeedItem>> componentSeeds;  // by coordinator component (Api::CoordinatorComponentId)
		std::vector<const RE::NiAVObject*> switches;
		bool switchResync = false;
		std::vector<std::shared_ptr<const void>> retiredImports;
		std::array<std::vector<Api::ClassMemberChange>, Extensions::kPublishedEntryClasses> classMembers;
		std::vector<LightEntryChange> lightEntries;
		std::vector<std::vector<std::shared_ptr<const void>>> componentDeltas;
	};

	/** @brief A pass that changed none of these publishes nothing. */
	struct PublishKey
	{
		std::array<std::uint64_t, 17> words{};
		std::array<std::uint64_t, 6> lookups{};
		const void* catalog = nullptr;
		bool operator==(const PublishKey&) const = default;
	};
	namespace Scene
	{
		/** @brief Latency histogram buckets in frames: 0, 1, 2, 3, 4-7, 8-15, 16-63, 64+. */
		inline constexpr std::size_t kAgeBuckets = 8;
		inline constexpr std::array<const char*, kAgeBuckets> kAgeBucketNames{ "0", "1", "2", "3", "4-7", "8-15", "16-63", "64+" };
		constexpr std::size_t AgeBucket(std::uint32_t a_frames)
		{
			return a_frames < 4 ? a_frames : a_frames < 8 ? 4 : a_frames < 16 ? 5 : a_frames < 64 ? 6 : 7;
		}
		/** @brief 0 past the column. */
		inline std::uint8_t PhasesIn(const SceneTables& a_tables, std::uint32_t a_slot) { return a_slot < a_tables.setPhases.size() ? a_tables.setPhases[a_slot] : std::uint8_t{ 0 }; }
		/** @brief Whether the pipeline is compiled into the set for the slot's current key. */
		inline bool PipelineDrawableIn(const SceneTables& a_tables, const Lookups& a_lookups, std::uint32_t a_pipeline)
		{
			return a_pipeline < a_tables.pipelines.size() && a_pipeline < a_lookups.pipelines.size() && a_lookups.pipelines[a_pipeline].setIndex != Lookups::kNone &&
			       a_lookups.pipelines[a_pipeline].key == a_tables.pipelines[a_pipeline];
		}
	}
}

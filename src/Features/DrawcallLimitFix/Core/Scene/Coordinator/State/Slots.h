#pragma once

// The slot tables, their indices by key, object indices, slot references and the geometry scan's index.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorSlots
	{
		SceneTables tables;
		// Bumping it invalidates every entry's object index at once.
		std::uint32_t objectStamp = 1;
		ankerl::unordered_dense::map<const RE::BSGraphics::TriShape*, std::uint32_t> geometryIndex;
		ankerl::unordered_dense::map<PipelineKey, std::uint32_t, PipelineKeyHash> pipelineIndex;
		ankerl::unordered_dense::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, std::uint32_t> materialIndex;
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::vector<std::uint32_t>> materialDependents;
		std::uint32_t tablesGeneration = 0;
		std::uint32_t materialVersions = 0;  // the last Tables::materialVersion handed out
		std::array<std::uint64_t, kChangeCauseCount> reportedChangeCounts{};
		std::vector<std::uint32_t> refreshedGeometry;  // geometry slots re-resolved in place this walk
		// Slot liveness, counted from the logs. Objects drawing a stale geometry slot are rewritten.
		struct SlotReferences
		{
			static constexpr std::uint32_t kNone = ~0u;
			struct Reference
			{
				std::uint32_t slot = kNone, generation = 0;
				bool operator==(const Reference&) const = default;
			};
			struct Counted
			{
				Reference geometry, pipeline, material;
			};
			LogCursor objects, links;
			std::vector<Counted> object;  // per object slot
			std::vector<Reference> link;  // per geometry slot
		} slotReferences;
		std::vector<std::uint32_t> freedGeometry;
		std::vector<std::uint32_t> staleGeometrySlots;
		// Geometry slots drawing a buffer DXVK moved. Their leases hold the old storage until they are resolved again.
		ankerl::unordered_dense::set<std::uint32_t> relocatedGeometrySlots;
		std::vector<std::uint32_t> relocatedGeometry;
		/**
		 * @brief Geometry slot to object slots, keys only and checked again at the scan. An entry may go stale but none is missing.
		 * A stale slot's objects are those under the heads its partition links lead back to.
		 */
		ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint32_t>> headObjects;
		std::vector<std::uint32_t> objectHead, objectHeadAt;  // per object slot: its head (kNoPartition: none) and its index under it
		ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint32_t>> linkedFrom;  // geometry slot: the slots linked to it
		bool geometryResolvedLastWalk = false;
		// Only a freed slot can leave a bound object pointing at a dead slot, so CheckObjectSlots runs only then.
		bool slotsFreedThisFrame = true;
		struct GeometrySource
		{
			const RE::BSGraphics::TriShape* key = nullptr;
			ID3D11Buffer* vertexBuffer = nullptr;
			ID3D11Buffer* indexBuffer = nullptr;
			std::uint64_t vertexDesc = 0;
			std::uint32_t vertexCount = 0;
			std::uint32_t indexCount = 0;
			std::uint32_t firstIndex = 0;  // an index range (object LOD's visible ranges); 0: the whole
			bool layer = false;            // a non-TriShape key (Tables::geometryLayerKey): a layer's second index list, particle quads
		};
	};
}

#pragma once

// The residents: the resident slots and their patches, and the pipelines and materials they keep alive.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorResidents
	{
		struct ResidentPatch
		{
			AccumulatedPass pass;
			std::uint32_t pipeline = 0, material = 0;
		};
		static constexpr std::uint32_t kNotResident = ~0u;
		std::vector<std::uint32_t> residents;
		std::vector<std::uint32_t> residentPos;
		std::vector<ResidentPatch> residentPatches;
		// Only slots NoteResidentTouched names are recounted. Rebuilt whole when every residency ended or the tables reset.
		bool residentMaintenanceDirty = true;
		/** @brief Each slot is in one list at a time. Removal swaps. */
		struct SlotMembers
		{
			std::vector<std::vector<std::uint32_t>> lists;
			std::vector<std::uint32_t> position;
			void Add(std::uint32_t a_key, std::uint32_t a_slot)
			{
				if (lists.size() <= a_key)
					lists.resize(std::size_t(a_key) + 1);
				if (position.size() <= a_slot)
					position.resize(std::size_t(a_slot) + 1, ~0u);
				position[a_slot] = static_cast<std::uint32_t>(lists[a_key].size());
				lists[a_key].push_back(a_slot);
			}
			void Remove(std::uint32_t a_key, std::uint32_t a_slot)
			{
				auto& list = lists[a_key];
				const std::uint32_t at = position[a_slot];
				const std::uint32_t moved = list.back();
				list[at] = moved;
				position[moved] = at;
				list.pop_back();
				position[a_slot] = ~0u;
			}
			std::uint32_t First(std::uint32_t a_key) const { return a_key < lists.size() && !lists[a_key].empty() ? lists[a_key].front() : ~0u; }
			std::size_t Count(std::uint32_t a_key) const { return a_key < lists.size() ? lists[a_key].size() : 0; }
			void Clear()
			{
				lists.clear();
				position.clear();
			}
			bool Has(std::uint32_t a_key, std::uint32_t a_slot) const
			{
				return a_key < lists.size() && a_slot < position.size() && position[a_slot] < lists[a_key].size() && lists[a_key][position[a_slot]] == a_slot;
			}
		};
		struct ResidentCounted
		{
			static constexpr std::uint32_t kNone = ~0u;
			std::uint32_t pipeline = kNone, material = kNone;
		};
		std::vector<ResidentCounted> residentCounted;
		std::vector<std::uint32_t> residentsTouched;   // since the last KeepResidentsAlive
		SlotMembers pipelineMembers;
		std::vector<std::uint32_t> materialMembers;    // by material slot: resident count
		// Cleared again where no resident holds them.
		std::vector<std::uint32_t> joinMarkedPipelines, joinMarkedMaterials;
		// Keeps its pipeline and material used so they get built and resolved before the switch.
		struct StagedBinding
		{
			std::uint32_t pipeline = 0, material = 0;
			std::uint32_t pipelineGeneration = 0, materialGeneration = 0;
		};
		ankerl::unordered_dense::map<std::uint32_t, StagedBinding> stagedBindings;
		std::vector<ResidentCounted> stagedCounted;
		SlotMembers stagedPipelineMembers;
		ankerl::unordered_dense::set<std::uint32_t> rebindingMembers;
		// Catches a move split across passes.
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> erasedByDetach;
		std::uint64_t splitMoves = 0;
		ankerl::unordered_dense::set<const RE::BSGeometry*> residentJoining;  // this pass's resident passes, until patched
		ankerl::unordered_dense::set<const RE::BSGeometry*> residentLayerJoining;  // the layers'
	};
}

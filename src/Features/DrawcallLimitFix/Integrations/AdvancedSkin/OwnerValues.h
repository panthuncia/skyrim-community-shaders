#pragma once

// Advanced Skin's owner index: one wetness value per actor, copied to its meshes. Render thread.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DCLF::Integrations::AdvancedSkin
{
	// Actor-owned slots grouped by actor, with one sampled value per actor copied to its slots. Built from published membership changes.
	// Ids include incarnations, and nothing here stores engine pointers. Consume only after a writer batch.
	class OwnerValues
	{
	public:
		using Value = std::array<float, 4>;
		struct Stats { std::uint32_t actors = 0, changed = 0, visited = 0, members = 0; };
		bool Contains(std::uint32_t slot, std::uint64_t group, std::uint64_t identity) const
		{
			return group && slot < slots.size() && slots[slot].group == group && slots[slot].identity == identity;
		}
		/** @brief Whether the slot belongs to an actor. */
		bool Grouped(std::uint32_t slot) const { return slot < slots.size() && slots[slot].group; }
		void Clear() { groups.clear(); groupIndex.clear(); slots.clear(); }
		/** @brief Sizes for the diagnostic capture: actors and indexed slots. */
		std::size_t GroupCount() const { return groups.size(); }
		std::size_t SlotCount() const { return slots.size(); }
		void Set(std::uint32_t slot, std::uint64_t group, std::uint64_t identity, bool resetValue = false)
		{
			if (slots.size() <= slot) slots.resize(std::size_t(slot) + 1);
			auto& old = slots[slot];
			if (old.group == group && old.identity == identity) {
				if (group && resetValue && !old.pending) {
					groups[groupIndex.at(group)].pending.push_back(slot);
					old.pending = true;
				}
				return;
			}
			if (old.group) {
				const auto at = groupIndex.at(old.group);
				auto& entry = groups[at];
				const auto moved = entry.members.back();
				entry.members[old.offset] = moved;
				slots[moved].offset = old.offset;
				entry.members.pop_back();
				if (old.pending) std::erase(entry.pending, slot);
				if (entry.members.empty()) {
					groupIndex.erase(old.group);
					if (at + 1 != groups.size()) {
						groups[at] = std::move(groups.back());
						groupIndex.at(groups[at].id) = at;
					}
					groups.pop_back();
				}
			}
			old = { group, identity };
			if (group) {
				auto [it, inserted] = groupIndex.try_emplace(group, groups.size());
				if (inserted) {
					groups.emplace_back();
					groups.back().id = group;
				}
				auto& entry = groups[it->second];
				old.offset = entry.members.size();
				old.pending = true;
				entry.members.push_back(slot);
				entry.pending.push_back(slot);
			}
		}
		template <class Sample, class Apply>
		Stats Update(Sample&& sample, Apply&& apply)
		{
			Stats stats;
			for (auto& entry : groups) {
				++stats.actors;
				stats.members += static_cast<std::uint32_t>(entry.members.size());
				const Value next = sample(*entry.members.begin());
				const bool changed = !entry.valid || std::memcmp(next.data(), entry.value.data(), sizeof(Value)) != 0;
				stats.changed += changed;
				// Only changed actors copy to all meshes. Joining or reinitialized meshes still need the current value even if it did not change.
				for (const auto slot : changed ? entry.members : entry.pending) {
					apply(slot, next);
					++stats.visited;
				}
				entry.value = next;
				entry.valid = true;
				for (const auto slot : entry.pending) slots[slot].pending = false;
				entry.pending.clear();
			}
			return stats;
		}
	private:
		struct Binding {
			std::uint64_t group = 0, identity = 0;
			std::size_t offset = 0;
			bool pending = false;
		};
		struct Group {
			std::uint64_t id = 0;
			std::vector<std::uint32_t> members, pending;
			Value value{};
			bool valid = false;
		};
		std::vector<Binding> slots;
		// Dense capture and fan-out storage. Hash lookups and membership upkeep only happen on writer events, never in the per-actor loop.
		std::vector<Group> groups;
		std::unordered_map<std::uint64_t, std::size_t> groupIndex;
	};
}

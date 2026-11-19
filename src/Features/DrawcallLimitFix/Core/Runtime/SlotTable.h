#pragma once

#include <cstdint>
#include <deque>
#include <set>
#include <vector>

namespace DCLF
{
	/**
	 * @brief The slots of one of SceneTables' shared tables, whose columns are parallel vectors grown together. Coordinator thread.
	 * A slot lives while something references it, and is freed after the batch in which its last reference went.
	 * Freeing a referenced slot moves its generation on, so counts taken against the old generation lapse instead of landing on the next use.
	 * A freed slot is retiring, because a publication may still name it. The owner recycles it once no such publication lives.
	 * Free slots are reused lowest first so the table's top falls as a scene unloads. Generations survive TrimTop.
	 */
	class SlotTable
	{
	public:
		struct Allocation
		{
			std::uint32_t slot = 0;
			bool grown = false;  // a new slot at the end, so the caller grows its columns
		};

		/** @brief A free or new slot. It is unreferenced, so the next drain frees it unless a reference comes. */
		Allocation Allocate()
		{
			Allocation allocation;
			if (!freeList.empty()) {
				allocation.slot = *freeList.begin();
				freeList.erase(freeList.begin());
			} else {
				allocation.slot = static_cast<std::uint32_t>(alive.size());
				allocation.grown = true;
				alive.push_back(0);
				refs.push_back(0);
				if (generation.size() < alive.size())
					generation.push_back(0);
			}
			alive[allocation.slot] = 1;
			refs[allocation.slot] = 0;
			++aliveCount;
			BecomeIdle(allocation.slot);
			return allocation;
		}
		/** @brief Frees a slot whatever references it has. Counts taken against it lapse. */
		void Free(std::uint32_t a_slot)
		{
			if (!Alive(a_slot))
				return;
			alive[a_slot] = 0;
			if (refs[a_slot])
				--referencedCount;
			refs[a_slot] = 0;
			++generation[a_slot];
			--aliveCount;
			retiring.push_back({ a_slot, generation[a_slot] });
		}
		/** @brief The slots freed since the last call, with their generations, for Recycle. */
		template <class F>
		void TakeRetiring(F&& a_take)
		{
			for (const auto& entry : retiring)
				a_take(entry.slot, entry.generation);
			retiring.clear();
		}
		/** @brief Puts a retired slot back on the free list once no publication names it, unless it was reused or cleared since. */
		void Recycle(std::uint32_t a_slot, std::uint32_t a_generation)
		{
			if (a_slot < alive.size() && !alive[a_slot] && generation[a_slot] == a_generation)
				freeList.insert(a_slot);
		}
		/** @brief Drops free slots past the last one in use, alive or retiring. Returns the new Size(), to which the owner cuts its columns. */
		std::size_t TrimTop()
		{
			while (!alive.empty() && !freeList.empty() && *freeList.rbegin() == alive.size() - 1) {
				freeList.erase(std::prev(freeList.end()));
				alive.pop_back();
				refs.pop_back();
			}
			return alive.size();
		}
		std::size_t FreeCount() const { return freeList.size(); }
		std::size_t Retiring() const { return retiring.size(); }
		bool Alive(std::size_t a_slot) const { return a_slot < alive.size() && alive[a_slot]; }
		std::uint32_t Generation(std::size_t a_slot) const { return a_slot < generation.size() ? generation[a_slot] : 0; }
		/** @brief A reference to the slot as of a_generation. Nothing if the slot has moved on since. */
		void AddRef(std::uint32_t a_slot, std::uint32_t a_generation)
		{
			if (!Alive(a_slot) || generation[a_slot] != a_generation)
				return;
			if (refs[a_slot]++ == 0)
				++referencedCount;
		}
		void Release(std::uint32_t a_slot, std::uint32_t a_generation)
		{
			if (!Alive(a_slot) || generation[a_slot] != a_generation || refs[a_slot] == 0)
				return;
			if (--refs[a_slot] == 0) {
				--referencedCount;
				BecomeIdle(a_slot);
			}
		}
		/** @brief Sets every count to zero before a recount. */
		void ResetReferences()
		{
			idle.clear();
			referencedCount = 0;
			for (std::uint32_t slot = 0; slot < alive.size(); ++slot) {
				refs[slot] = 0;
				if (alive[slot])
					BecomeIdle(slot);
			}
		}
		/**
		 * @brief After a complete batch of reference changes, frees each slot whose last reference went, calling a_onFree first.
		 * A reference reacquired in the batch cancels the free, and stale events for a reused slot are skipped by generation.
		 */
		template <class F>
		std::uint32_t DrainUnreferenced(F&& a_onFree)
		{
			std::uint32_t freed = 0;
			while (!idle.empty()) {
				const auto entry = idle.front();
				idle.pop_front();
				if (!Alive(entry.slot) || generation[entry.slot] != entry.generation || refs[entry.slot])
					continue;
				a_onFree(entry.slot);
				Free(entry.slot);
				++freed;
			}
			return freed;
		}
		std::size_t Size() const { return alive.size(); }
		std::size_t AliveCount() const { return aliveCount; }
		std::size_t ReferencedCount() const { return referencedCount; }
		std::uint32_t References(std::size_t a_slot) const { return a_slot < refs.size() ? refs[a_slot] : 0; }
		void Clear() { *this = {}; }

	private:
		void BecomeIdle(std::uint32_t a_slot) { idle.push_back({ a_slot, generation[a_slot] }); }

		struct Idle
		{
			std::uint32_t slot = 0, generation = 0;
		};
		std::vector<std::uint8_t> alive;
		std::vector<std::uint32_t> refs, generation;  // generation is at least alive's size and survives TrimTop
		std::set<std::uint32_t> freeList;            // lowest first
		std::vector<Idle> retiring;  // freed, not yet on the free list
		std::deque<Idle> idle;  // last-reference events, in order
		std::size_t aliveCount = 0, referencedCount = 0;
	};
}

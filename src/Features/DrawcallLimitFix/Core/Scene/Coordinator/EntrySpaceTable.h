#pragma once

// The coordinator's stable index space for a candidate set, and the pooled snapshots published from it. Coordinator thread only.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Scene/EntrySpace.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"

namespace DCLF::Scene
{
	/**
	 * @brief Entry and geometry indices with their free lists, and a version per entry. Publish brings a pooled snapshot no reader
	 * holds up to date at the indices whose versions moved.
	 *
	 * After each publication a spare snapshot is caught up on the preparation pool, so the lane only brings up one publication's run.
	 */
	struct EntrySpaceTable
	{
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> index;
		std::vector<const RE::NiAVObject*> nodes;
		std::vector<std::uint32_t> version;
		std::vector<std::vector<std::uint32_t>> geometries;  // per entry
		std::vector<std::uint32_t> free;
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> geometryIndex;
		std::vector<const RE::BSGeometry*> geometryNodes;
		std::vector<std::uint32_t> geometryEntry;
		std::vector<std::int32_t> geometrySlot;
		std::vector<std::uint8_t> primaryGeometry;
		std::vector<std::uint32_t> geometryFree;
		struct Pooled
		{
			/** @brief A new one per dispatch, so a stale job that finds it claimed by the lane does nothing. */
			struct State
			{
				std::atomic<bool> claimed{ false };  // taken by the job or by the lane (whichever first runs the catch-up)
				std::atomic<bool> busy{ false };     // the catch-up is not done (released once it is: the snapshot's writes with it)
				std::uint64_t log = 0;               // the snapshot holds this log's generation below (0: none a run chains from)
				std::uint32_t generation = 0;
				std::uint32_t caughtUp = 0;  // indices the catch-up brought up (the lane counts them as it takes the spare)
			};
			std::shared_ptr<EntrySpace> snapshot = std::make_shared<EntrySpace>();
			LogCursor cursor;
			std::shared_ptr<State> state = std::make_shared<State>();
			// Held by the lane until the catch-up is done, since the job reads it by address.
			std::shared_ptr<const EntrySpace> source;
		};
		std::vector<Pooled> pool;
		// Reset invalidates it, so every reader is brought up whole.
		EventLog<std::uint32_t> log;
		std::uint64_t logId = NextLogId();
		std::uint64_t published = 0;  // log's end at the last publication (the next run starts there)
		std::shared_ptr<const EntrySpaceChanges> changes;  // the last publication's
		std::shared_ptr<const EntrySpace> last;        // the last publication (parity's reference; the coordinator holds it too)
		std::vector<std::uint8_t> marks;  // per index, Publish's scratch
		bool changed = true;                              // an index moved since the last snapshot
		std::uint64_t entriesWritten = 0, snapshots = 0;  // since the last report
		std::uint64_t rowsTakenBack = 0;  // since the last report: entries written again for a geometry another let go of (released)
		std::uint64_t entriesCaughtUp = 0, wholeCatchUps = 0;  // since the last report: indices a snapshot was brought up at; snapshots brought up index by index
		std::uint64_t entriesKept = 0, spareCatchUps = 0, spareEntries = 0, sparesRunByLane = 0, sparesWaited = 0, poolGrowths = 0;
		std::uint64_t poolTrimmed = 0;  // since the last report: free snapshots let go of past kFreeSpares (TrimPool)
		std::vector<std::uint8_t> setKept;        // per position, Set's scratch
		std::vector<std::uint32_t> setPrevious;  // Set's scratch: the entry's geometries before
		// A geometry moved between entries in one update may be skipped by the new entry's Set. The caller writes that entry again.
		std::vector<const RE::BSGeometry*> released;
		// The only references a candidate set holds, since its snapshots hold none. Replaced roots go to retiredRoots for the caller to retire.
		std::vector<RE::NiPointer<RE::NiAVObject>> owned;
		std::vector<RE::NiPointer<RE::NiRefObject>> retiredRoots;

		static std::uint64_t NextLogId();

		void Reset(std::uint32_t& a_versions);
		std::vector<const RE::BSGeometry*> TakeReleased()
		{
			auto taken = std::exchange(released, {});
			std::erase_if(taken, [this](const RE::BSGeometry* a_geometry) { return geometryIndex.contains(a_geometry); });
			return taken;
		}
		/** @brief a_dependents null means not a candidate. a_geometry(geometry) gives a new geometry's object slot and primary verdict. */
		template <class Geometry>
		void Set(const RE::NiAVObject* a_root, const std::vector<RE::BSGeometry*>* a_dependents, std::uint32_t& a_versions, Geometry&& a_geometry);
		/** @brief Roots let go of are left in retiredRoots. */
		std::shared_ptr<const EntrySpace> Publish(std::uint32_t& a_generation, std::uint64_t a_publication,
			const std::function<RE::NiPointer<RE::NiAVObject>(const RE::NiAVObject*)>& a_ownedRoot);
		/** @brief The last Publish's a_publication is the first not to name them. */
		std::vector<RE::NiPointer<RE::NiRefObject>> TakeRetiredRoots() { return std::exchange(retiredRoots, {}); }
		/** @brief Teardown only. */
		void ReleaseOwned();

		EntrySpaceTable() = default;
		EntrySpaceTable(const EntrySpaceTable&) = delete;
		EntrySpaceTable& operator=(const EntrySpaceTable&) = delete;
		/** @brief Waits for every spare's catch-up, since the job reads the pool's snapshots by address. */
		~EntrySpaceTable();

	private:
		static constexpr std::size_t kFreeSpares = 2;

		void DispatchSpare(std::size_t a_published);
		/** @brief The pool grows while every snapshot is held. A free one is only memory, so extras past kFreeSpares are let go. */
		void TrimPool();
		/** @brief Runs the job here if it has not started, else waits for it. */
		void FinishSpare(Pooled& a_pooled);
	};

	template <class Geometry>
	void EntrySpaceTable::Set(const RE::NiAVObject* a_root, const std::vector<RE::BSGeometry*>* a_dependents, std::uint32_t& a_versions, Geometry&& a_geometry)
	{
		const auto found = index.find(a_root);
		if (!a_dependents && found == index.end())
			return;
		std::uint32_t e;
		if (found != index.end()) {
			e = found->second;
		} else if (!free.empty()) {
			e = free.back();
			free.pop_back();
		} else {
			e = static_cast<std::uint32_t>(nodes.size());
			nodes.push_back(nullptr);
			version.push_back(0);
			geometries.emplace_back();
		}
		// A geometry listed again at the same position keeps its index and map entry, so a rewrite moves none of them.
		auto& list = geometries[e];
		setKept.assign(list.size(), 0);
		if (a_dependents)
			for (std::size_t k = 0, n = std::min(list.size(), a_dependents->size()); k < n; ++k)
				setKept[k] = geometryNodes[list[k]] == (*a_dependents)[k] ? 1 : 0;
		for (std::size_t k = 0; k < list.size(); ++k) {
			if (setKept[k])
				continue;
			const std::uint32_t g = list[k];
			if (const auto it = geometryIndex.find(geometryNodes[g]); it != geometryIndex.end() && it->second == g) {
				geometryIndex.erase(it);
				released.push_back(geometryNodes[g]);
			}
			geometryNodes[g] = nullptr;
			geometryEntry[g] = EntrySpace::kNone;
			geometryFree.push_back(g);
		}
		version[e] = ++a_versions;
		log.Push(e);
		changed = true;
		++entriesWritten;
		if (!a_dependents) {
			list.clear();
			index.erase(found);
			nodes[e] = nullptr;
			free.push_back(e);
			return;
		}
		nodes[e] = a_root;
		index.insert_or_assign(a_root, e);
		auto& previous = setPrevious;
		previous.swap(list);
		list.clear();
		for (std::size_t k = 0; k < a_dependents->size(); ++k) {
			auto* geometry = (*a_dependents)[k];
			std::uint32_t g;
			if (k < previous.size() && setKept[k]) {
				g = previous[k];
			} else {
				if (geometryIndex.contains(geometry))
					continue;  // listed by another entry first (or by this one, kept at another position)
				if (!geometryFree.empty()) {
					g = geometryFree.back();
					geometryFree.pop_back();
				} else {
					g = static_cast<std::uint32_t>(geometryNodes.size());
					geometryNodes.push_back(nullptr);
					geometryEntry.push_back(EntrySpace::kNone);
					geometrySlot.push_back(-1);
					primaryGeometry.push_back(0);
				}
				geometryIndex.emplace(geometry, g);
				geometryNodes[g] = geometry;
				geometryEntry[g] = e;
			}
			std::tie(geometrySlot[g], primaryGeometry[g]) = a_geometry(geometry);
			list.push_back(g);
		}
	}
}

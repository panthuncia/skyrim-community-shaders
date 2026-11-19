#pragma once

// A published entry space, the sun's or the point lights' candidate set, and its change log. Immutable once published, any thread.

#include <cstdint>
#include <memory>
#include <vector>

#include <ankerl/unordered_dense.h>


namespace DCLF
{
	struct EntrySpaceChanges;

	/**
	 * @brief The entries DCLF could take out of the engine's culls. An entry is a candidate when every tracked geometry under it
	 * is a table object or one the engine never draws into a shadow map.
	 *
	 * Indices are stable while an entry is a candidate. Every index change takes a session-unique version, so snapshots differ
	 * exactly where versions differ.
	 */
	struct EntrySpace
	{
		EntrySpace() = default;
		EntrySpace(const EntrySpace&) = delete;
		EntrySpace& operator=(const EntrySpace&) = delete;

		std::uint32_t generation = 0;
		// The entry nodes live while EngineReleases::Alive(publication). The snapshot holds none, so keeping it keeps no engine object alive.
		std::uint64_t publication = 0;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> entries;     // entry node -> entry index
		std::vector<const RE::NiAVObject*> entryNodes;                                  // entry index -> entry node (null: free)
		std::vector<std::uint32_t> entryVersion;                                        // entry index -> its version
		std::vector<std::vector<std::uint32_t>> entryGeometries;                        // entry index -> its geometry indices
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> geometries;  // every tracked geometry under one -> geometry index
		std::vector<const RE::BSGeometry*> geometryNodes;                               // geometry index -> geometry (null: free)
		std::vector<std::uint32_t> geometryEntry;                                       // geometry index -> entry index (kNone: free)
		// -1 when there is no record. The frame reads this, not the tracked set.
		std::vector<std::int32_t> geometrySlot;
		// Per geometry, Eligibility::PrimaryVerdict.
		std::vector<std::uint8_t> primaryGeometry;
		// Which indices moved since earlier generations. Null for a snapshot no table published.
		std::shared_ptr<const EntrySpaceChanges> changes;

		static constexpr std::uint32_t kNone = ~0u;
		/** @brief Entry indices are below it, free ones among them. */
		std::uint32_t Capacity() const { return static_cast<std::uint32_t>(entryNodes.size()); }
		std::uint32_t GeometryCapacity() const { return static_cast<std::uint32_t>(geometryNodes.size()); }
		std::uint32_t Count() const { return static_cast<std::uint32_t>(entries.size()); }
	};

	/** @brief A table's change log as one snapshot carries it. Immutable once published, any thread. */
	struct EntrySpaceChanges
	{
		/** @brief The indices written in one generation, ascending, each once. */
		struct Run
		{
			std::uint32_t generation = 0;
			std::vector<std::uint32_t> entries;
		};
		std::uint64_t log = 0;         // session-unique per table: snapshots of different tables never chain
		std::uint32_t floor = 0;       // the runs reach a snapshot of a generation from floor on (Reset, the trim: floor moves on)
		std::uint32_t generation = 0;  // the snapshot's
		std::vector<std::shared_ptr<const Run>> runs;  // oldest first, contiguous over (floor, generation]

		bool Reaches(std::uint64_t a_log, std::uint32_t a_generation) const { return a_log == log && a_generation >= floor && a_generation <= generation; }
	};

	/** @brief Compares every index. Readers that update by change should use ChangedEntriesByLog. */
	void ChangedEntries(const EntrySpace* a_from, const EntrySpace& a_to, std::vector<std::uint32_t>& a_out);
	/**
	 * @brief Reads the log's runs when a_to's log reaches a_from, else compares index by index. Gives the same result as
	 * ChangedEntries. Returns whether the log answered. Any thread.
	 */
	bool ChangedEntriesByLog(const EntrySpace* a_from, const EntrySpace& a_to, std::vector<std::uint32_t>& a_out);
	void ChangedEntriesByScan(const EntrySpace* a_from, const EntrySpace& a_to, std::vector<std::uint32_t>& a_out);
}

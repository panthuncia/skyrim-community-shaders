#pragma once

// Index ranges an entry draws instead of its whole index list, such as object LOD's visible segments.
// One whole range uses the entry's own geometry slot, no ranges draw nothing, and anything else becomes a chain of range slots.

#include <cstdint>
#include <vector>

namespace DCLF::Api
{
	/** @brief One drawn index range: DrawIndexed(indexCount, firstIndex, 0) over the entry's renderer data. */
	struct EntryRange
	{
		std::uint32_t firstIndex = 0;
		std::uint32_t indexCount = 0;
		// The range's geometry slot key: its first segment's record, unique while the shape lives. Null for a whole range.
		const void* key = nullptr;
		// Compares triangles only. When the engine rebuilds the runs (FUN_140e31180), the same range can come back with another key.
		bool operator==(const EntryRange& a_other) const { return firstIndex == a_other.firstIndex && indexCount == a_other.indexCount; }
	};

	/** @brief Whether a_ranges is the entry's whole index list as one range. */
	inline bool EntryRangesWhole(std::uint32_t a_triangles, const std::vector<EntryRange>& a_ranges)
	{
		return a_ranges.size() == 1 && a_ranges[0].firstIndex == 0 && a_ranges[0].indexCount == a_triangles * 3u;
	}
}

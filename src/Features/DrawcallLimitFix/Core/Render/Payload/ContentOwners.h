#pragma once

// Tracks which descriptors the bytes in GPU buffers currently name, by byte range.
// Bytes written once can be read by many later executions, so each execution holds a snapshot of the owners
// to keep those descriptors alive while the bytes are current.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace DCLF::Draws
{
	/**
	 * @brief Owners of uploaded descriptor-naming bytes. Render thread only.
	 * Versioned buffers are keyed by Key(), so growth copies keep their owners.
	 */
	class ContentOwners
	{
	public:
		/** @brief Records the owners for a range, replacing every range it overlaps. Replaced owners live until the next snapshot is released. */
		void Set(const void* a_target, std::uint64_t a_offset, std::uint64_t a_bytes, std::span<const std::shared_ptr<const void>> a_owners);

		/** @brief One owner holding all current and recently replaced owners. Rebuilt only after a change. Null when empty. */
		std::shared_ptr<const void> Snapshot();

		struct Counts
		{
			std::size_t ranges = 0, owners = 0;
			std::uint64_t snapshotsMade = 0;
		};
		Counts TakeCounts();

	private:
		struct Range
		{
			std::uint64_t bytes = 0;
			std::vector<std::shared_ptr<const void>> owners;
		};
		std::map<std::pair<const void*, std::uint64_t>, Range> ranges;
		std::vector<std::shared_ptr<const void>> replaced;  // owners of ranges replaced since the last snapshot
		std::shared_ptr<const void> sealed;
		bool changed = false;
		std::uint64_t snapshotsMade = 0;
	};
}

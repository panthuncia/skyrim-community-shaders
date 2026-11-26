#pragma once

// CS's side of ORG's resource relocation. A relocation keeps a resource's identity but gives its backing new memory, and
// in steady state new bindless indices. Code that caches an index, or hands out something only the old backing has, uses these helpers.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace org
{
	class GloballyIndexedResource;
	class Resource;
}

namespace RenderGraph::Relocation
{
	/**
	 * @brief Marks a_resource Pinned: its backing never moves. For a resource whose device address, buffer view ref or raw
	 * handle was handed out, or that CS made raw views of. Pinned stays: Seal leaves a pinned resource alone. A resource with
	 * no backing of its own (an import) is skipped. Any thread.
	 */
	void Pin(const org::Resource& a_resource);

	/**
	 * @brief Marks a_resource Written: ORG moves it only while the GPU is idle (a defragmentation at a loading screen), and CS
	 * follows the move through the relocation journal (FollowBufferMoves). For a buffer whose device address or buffer view ref
	 * went out to code that follows: a view ref keeps its slot through an idle move, and an address holder registers with
	 * AddBufferMovesListener. A pinned resource stays pinned, also when ORG pinned it by its memory (mapped or shared heaps).
	 * Skipped for a resource with no backing yet or none of its own. Seal leaves it Written. Any thread.
	 */
	void FollowAtIdle(const org::Resource& a_resource);

	/**
	 * @brief Marks a_resource ReadOnly: nothing writes it any more, so ORG may move it at any time (and the
	 * ORG_RELOCATION_STRESS switch picks it). Call it once the last write was submitted, from the thread that owns the
	 * resource. Skipped for a pinned resource, for one marked FollowAtIdle (a move while the GPU runs would leave its
	 * followers behind) and for one with no backing of its own (imports, exports).
	 */
	void Seal(const std::shared_ptr<org::Resource>& a_resource);

	/**
	 * @brief Adds a_listener, called by FollowBufferMoves after a commit that moved buffers with device addresses (or when
	 * journal entries were lost, so buffers may have moved). A listener refreshes the addresses it caches and keeps work made
	 * with the old ones from the GPU. It runs on FollowBufferMoves' thread and must not throw. Any thread.
	 */
	void AddBufferMovesListener(std::function<void()> a_listener);

	/**
	 * @brief Reads the relocation journal and tells the listeners when buffers moved. Call it once a frame on the thread that
	 * seals the frame, right after the host's relocation commit (GraphHost::SealSubmitted). Not for concurrent use.
	 */
	void FollowBufferMoves();

	/** @brief The journal's head: the sequence its next entry gets. Keep it beside a cached index to ask MovedSince later. Any thread. */
	std::uint64_t JournalHead();

	/**
	 * @brief Whether the journal moved a_resourceID at or after sequence a_from, or lost entries from there (then it may have).
	 * For a rare check, such as a mismatch already found: it copies the entries. Any thread.
	 */
	bool MovedSince(std::uint64_t a_from, std::uint64_t a_resourceID);

	/**
	 * @brief A cursor of its own over ORG's relocation journal. It starts at the journal's head, so it sees only moves made after
	 * it was made.
	 */
	class JournalCursor
	{
	public:
		JournalCursor();

		/**
		 * @brief Appends the IDs of the resources moved since the last read to a_moved, and moves the cursor to the head. Returns
		 * false when entries were lost: the caller then refreshes everything it caches. Not for concurrent use of one cursor.
		 */
		bool Read(std::vector<std::uint64_t>& a_moved);

		/** @brief Whether the journal has entries this cursor has not read. Not concurrent with Read. */
		bool Behind() const;

	private:
		std::uint64_t cursor = 0;
	};

	/**
	 * @brief Bindless indices cached by resource. A holder tracks each cached index with the resource it names; Refresh, once
	 * a frame, reads the journal and stores the moved resources' new whole-resource SRV index (GetSRVInfo(0).slot.index) into
	 * their cells. When entries were lost, it refreshes every cell. A cell or resource that is gone is dropped.
	 * Track and Refresh may run on different threads. Readers of a cell load it.
	 */
	class IndexCache
	{
	public:
		using Cell = std::atomic<std::uint32_t>;

		/** @brief Stores a_resource's current index into a_cell and keeps it current from now on, while both live. */
		void Track(const std::shared_ptr<const org::GloballyIndexedResource>& a_resource, const std::shared_ptr<Cell>& a_cell);

		/** @brief Reads the journal and refreshes the cells of moved resources. Call it once a frame, after the frame's relocations were committed. */
		void Refresh();

	private:
		struct Entry
		{
			std::weak_ptr<const org::GloballyIndexedResource> resource;
			std::weak_ptr<Cell> cell;
		};

		/** @brief Stores the resource's index into the cell. False when either is gone. */
		static bool Store(const Entry& a_entry);

		std::mutex mutex;
		JournalCursor cursor;
		std::unordered_multimap<std::uint64_t, Entry> entries;  // by global resource ID
		std::size_t pruneAt = 64;                                 // entries count at which Track drops gone entries
		std::vector<std::uint64_t> moved;                         // Refresh's scratch
	};
}

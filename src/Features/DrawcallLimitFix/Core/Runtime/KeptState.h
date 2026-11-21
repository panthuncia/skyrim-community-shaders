#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// The building blocks of DCLF's persistent draw state, described in dclf-architecture.md, "Persistent draw state":
// the logs the tables write as they change, and the arrays kept across frames that GPU buffers mirror.
// Every kept structure uses these instead of its own copy of the protocol.
namespace DCLF
{
	// Whether kept arrays reuse their storage across builds. Only the CPU tests turn it off, to compare against fresh storage.
	inline bool reuseKeptStorage = true;
	/**
	 * @brief An append-only log with absolute positions, where entry k is at base + k. Readers keep their own position.
	 * A reader left behind the base by a trim or an invalidate reads everything again.
	 */
	template <class E>
	struct EventLog
	{
		std::vector<E> entries;
		std::uint64_t base = 0;

		std::uint64_t End() const { return base + entries.size(); }
		std::size_t Size() const { return entries.size(); }
		void Push(const E& a_entry) { entries.push_back(a_entry); }
		/** @brief Whether a reader at a_position can read on. */
		bool Readable(std::uint64_t a_position) const { return a_position >= base && a_position <= End(); }
		/** @brief The entries from a readable a_position on. */
		std::span<const E> From(std::uint64_t a_position) const
		{
			return std::span<const E>(entries).subspan(static_cast<std::size_t>(a_position - base));
		}
		/** @brief Makes every reader read everything again, by leaving a gap past the end. */
		void Invalidate()
		{
			base += entries.size() + 1;
			entries.clear();
		}
		/** @brief Drops the older half once the log holds more than a_max entries. */
		void Trim(std::size_t a_max)
		{
			if (entries.size() <= a_max)
				return;
			const std::size_t half = entries.size() / 2;
			entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(half));
			base += half;
		}
	};

	/**
	 * @brief An EventLog whose entries sit in fixed chunks its copies share, so copy, assign and trim cost O(chunks).
	 * A copy's entries never change. Only the copy at a chunk's end appends to it in place, claimed atomically.
	 * Any other copy appending there takes its own copy of the chunk first.
	 */
	template <class E>
	struct SharedEventLog
	{
		static_assert(std::is_trivially_copyable_v<E>);
		static constexpr std::size_t kChunk = 4096;
		struct Chunk
		{
			std::array<E, kChunk> entries;
			std::atomic<std::uint32_t> filled{ 0 };  // entries appended. A copy whose end reaches it appends in place.
		};
		/**
		 * @brief Entries [first, first + count) across the chunks. It reads the chunk list, not storage, so the reader may append while iterating.
		 * Trim or Invalidate while iterating is not allowed.
		 */
		struct LogRange
		{
			const std::vector<std::shared_ptr<Chunk>>* chunks = nullptr;
			std::size_t first = 0, count = 0;

			struct iterator
			{
				const std::vector<std::shared_ptr<Chunk>>* chunks = nullptr;
				std::size_t at = 0;
				const E& operator*() const { return (*chunks)[at / kChunk]->entries[at % kChunk]; }
				iterator& operator++()
				{
					++at;
					return *this;
				}
				bool operator==(const iterator& a_other) const { return at == a_other.at; }
			};
			iterator begin() const { return { chunks, first }; }
			iterator end() const { return { chunks, first + count }; }
			std::size_t size() const { return count; }
			bool empty() const { return count == 0; }
		};

		std::vector<std::shared_ptr<Chunk>> chunks;
		std::uint64_t base = 0;
		std::size_t head = 0;   // the base entry's index in the first chunk
		std::size_t count = 0;

		std::uint64_t End() const { return base + count; }
		std::size_t Size() const { return count; }
		void Push(const E& a_entry)
		{
			const std::size_t at = head + count;
			const std::size_t c = at / kChunk, slot = at % kChunk;
			if (c == chunks.size()) {
				chunks.push_back(std::make_shared<Chunk>());
				chunks.back()->filled.store(1, std::memory_order_relaxed);
			} else if (auto expected = static_cast<std::uint32_t>(slot);
					   !chunks[c]->filled.compare_exchange_strong(expected, static_cast<std::uint32_t>(slot + 1), std::memory_order_relaxed)) {
				// Another copy appended past this one's end here, so take a private copy of the chunk.
				auto own = std::make_shared<Chunk>();
				std::copy_n(chunks[c]->entries.begin(), slot, own->entries.begin());
				own->filled.store(static_cast<std::uint32_t>(slot + 1), std::memory_order_relaxed);
				chunks[c] = std::move(own);
			}
			chunks[c]->entries[slot] = a_entry;
			++count;
		}
		bool Readable(std::uint64_t a_position) const { return a_position >= base && a_position <= End(); }
		LogRange From(std::uint64_t a_position) const
		{
			const auto skip = static_cast<std::size_t>(a_position - base);
			return { &chunks, head + skip, count - skip };
		}
		void Invalidate()
		{
			base += count + 1;
			chunks.clear();
			head = 0;
			count = 0;
		}
		/** @brief Drops the older half once the log holds more than a_max entries. A copy holding the chunks keeps them. */
		void Trim(std::size_t a_max)
		{
			if (count <= a_max)
				return;
			const std::size_t half = count / 2;
			const std::size_t at = head + half;
			chunks.erase(chunks.begin(), chunks.begin() + static_cast<std::ptrdiff_t>(at / kChunk));
			head = at % kChunk;
			base += half;
			count -= half;
		}
		/** @brief Same base and entries. */
		bool operator==(const SharedEventLog& a_other) const
		{
			if (base != a_other.base || count != a_other.count)
				return false;
			auto left = From(base), right = a_other.From(base);
			for (auto l = left.begin(), r = right.begin(); l != left.end(); ++l, ++r)
				if (std::memcmp(&*l, &*r, sizeof(E)) != 0)
					return false;
			return true;
		}
	};

	/**
	 * @brief A reader's place in a log, and the tables generation it read from.
	 * It continues only in the same generation and while its position is readable. Otherwise it rebuilds from the tables.
	 */
	struct LogCursor
	{
		bool active = false;
		std::uint32_t generation = 0;
		std::uint64_t position = 0;

		template <class Log>
		bool Continues(const Log& a_log, std::uint32_t a_generation) const
		{
			return active && generation == a_generation && a_log.Readable(position);
		}
		void Restart(std::uint32_t a_generation)
		{
			active = true;
			generation = a_generation;
		}
		template <class Log>
		auto Unread(const Log& a_log) const
		{
			using Range = decltype(a_log.From(position));
			return a_log.Readable(position) ? a_log.From(position) : Range{};
		}
		template <class Log>
		void Advance(const Log& a_log)
		{
			position = a_log.End();
		}
	};

	/**
	 * @brief What changed in a kept array, by content epoch, for the buffers that mirror it.
	 * Each build that changes anything is one new content epoch. A buffer at some epoch is sent the ranges changed since, coalesced.
	 * A buffer below the floor, or a new one at 0, is sent everything. BeginBuild trims what the oldest counted holder already has.
	 */
	class ChangeJournal
	{
	public:
		struct Entry
		{
			std::uint64_t contentEpoch = 0;
			std::uint64_t first = 0;
			std::uint64_t count = 0;
		};

		/** @brief What a build's payload carries: the content epoch and the changes since the floor. */
		struct Snapshot
		{
			std::uint64_t contentEpoch = 0;  // 0 means not a kept array's, so sent whole
			std::uint64_t floor = 0;
			std::vector<Entry> entries;  // in content epoch order, which ForEachRun relies on

			bool Holds(std::uint64_t a_contentEpoch) const { return contentEpoch && a_contentEpoch == contentEpoch; }
			/** @brief Calls a_run(first, count) for every coalesced run a holder at a_contentEpoch lacks. Returns the elements sent. */
			template <class Run>
			std::uint64_t ForEachRun(std::uint64_t a_contentEpoch, std::uint64_t a_count, Run&& a_run) const
			{
				return Runs(contentEpoch, floor, entries, a_contentEpoch, a_count, a_run);
			}
			void Reset() { *this = {}; }
		};

		static constexpr std::size_t kMaxEntries = 1u << 16;

		/** @brief A build's start. Forgets changes the oldest counted holder already has, and opens the build. */
		void BeginBuild(std::uint64_t a_oldestContentEpoch)
		{
			// Clamped to the current epoch. ~0 means no holder holds anything, so every change goes.
			a_oldestContentEpoch = std::min(a_oldestContentEpoch, contentEpoch);
			if (a_oldestContentEpoch > floor) {
				std::erase_if(entries, [&](const Entry& a_entry) { return a_entry.contentEpoch <= a_oldestContentEpoch; });
				floor = a_oldestContentEpoch;
			}
			if (entries.size() > kMaxEntries) {
				entries.clear();
				floor = contentEpoch;
			}
			changed = false;
		}
		/** @brief Marks one element changed. Once per build however often it is marked. */
		void Mark(std::uint64_t a_index)
		{
			Touch();
			if (markedAt.size() <= a_index)
				markedAt.resize(static_cast<std::size_t>(a_index) + 1, 0);
			if (markedAt[a_index] == contentEpoch)
				return;
			markedAt[a_index] = contentEpoch;
			entries.push_back({ contentEpoch, a_index, 1 });
		}
		void MarkRange(std::uint64_t a_first, std::uint64_t a_count)
		{
			if (!a_count)
				return;
			Touch();
			entries.push_back({ contentEpoch, a_first, a_count });
		}
		/** @brief Everything changed. The epoch counts on, so no old epoch is reused. */
		void Resync()
		{
			Touch();
			entries.clear();
			floor = contentEpoch;
		}
		std::uint64_t ContentEpoch() const { return contentEpoch; }
		bool Changed() const { return changed; }
		Snapshot Take() const { return { contentEpoch, floor, entries }; }
		/** @brief ForEachRun on the journal itself without copying. Only valid between builds. */
		template <class Run>
		std::uint64_t ForEachRun(std::uint64_t a_contentEpoch, std::uint64_t a_count, Run&& a_run) const
		{
			return Runs(contentEpoch, floor, entries, a_contentEpoch, a_count, a_run);
		}

	private:
		template <class Run>
		static std::uint64_t Runs(std::uint64_t a_epoch, std::uint64_t a_floor, std::span<const Entry> a_entries, std::uint64_t a_contentEpoch,
			std::uint64_t a_count, Run&& a_run)
		{
			if (!a_count || (a_epoch && a_contentEpoch == a_epoch))
				return 0;
			if (!a_epoch || a_contentEpoch < a_floor || a_contentEpoch > a_epoch) {
				a_run(std::uint64_t{ 0 }, a_count);
				return a_count;
			}
			// Entries are in epoch order, so the holder lacks everything from the first entry newer than it.
			const auto newer = a_entries.subspan(static_cast<std::size_t>(std::partition_point(a_entries.begin(), a_entries.end(),
															 [&](const Entry& a_entry) { return a_entry.contentEpoch <= a_contentEpoch; }) -
														 a_entries.begin()));
			// Dense dirty tables use a bitmap to avoid sorting thousands of single-element entries. Sparse or large arenas sort ranges.
			if (reuseKeptStorage && newer.size() >= 128 && a_count <= (1u << 20) && a_count <= newer.size() * 64) {
				// The thread's scratch, taken for the call. A nested call makes its own.
				auto bits = std::exchange(BitsScratch(), {});
				bits.assign(static_cast<std::size_t>((a_count + 63) / 64), 0);
				for (const auto& entry : newer) {
					if (!entry.count || entry.first >= a_count)
						continue;
					const auto lastIndex = entry.first + std::min(entry.count, a_count - entry.first) - 1;
					const auto firstWord = entry.first / 64, lastWord = lastIndex / 64;
					const auto head = ~std::uint64_t{0} << (entry.first % 64);
					const auto tail = ~std::uint64_t{0} >> (63 - lastIndex % 64);
					if (firstWord == lastWord)
						bits[firstWord] |= head & tail;
					else {
						bits[firstWord] |= head;
						for (auto word = firstWord + 1; word < lastWord; ++word)
							bits[word] = ~std::uint64_t{0};
						bits[lastWord] |= tail;
					}
				}
				std::uint64_t first = 0, end = 0, sent = 0;
				for (std::size_t i = 0; i < bits.size(); ++i) {
					auto word = bits[i];
					while (word) {
						const auto startBit = std::countr_zero(word);
						const auto endBit = startBit + std::countr_one(word >> startBit);
						const std::uint64_t start = i * 64 + startBit;
						if (start != end) {
							if (first < end) { a_run(first, end - first); sent += end - first; }
							first = start;
						}
						end = i * 64 + endBit;
						word = endBit == 64 ? 0 : word & (~std::uint64_t{0} << endBit);
					}
				}
				if (first < end) { a_run(first, end - first); sent += end - first; }
				BitsScratch() = std::move(bits);
				return sent;
			}
			auto runs = std::exchange(RunsScratch(), {});
			runs.clear();
			runs.reserve(newer.size());
			for (const auto& entry : newer)
				runs.emplace_back(entry.first, entry.first + entry.count);
			std::sort(runs.begin(), runs.end());
			std::uint64_t sent = 0;
			for (std::size_t k = 0; k < runs.size();) {
				const std::uint64_t first = runs[k].first;
				std::uint64_t end = runs[k].second;
				while (++k < runs.size() && runs[k].first <= end)
					end = std::max(end, runs[k].second);
				end = std::min(end, a_count);
				if (first < end) {
					a_run(first, end - first);
					sent += end - first;
				}
			}
			RunsScratch() = std::move(runs);
			return sent;
		}
		// Per-thread scratch for runs, so nothing allocates once warm.
		static std::vector<std::uint64_t>& BitsScratch()
		{
			static thread_local std::vector<std::uint64_t> bits;
			return bits;
		}
		static std::vector<std::pair<std::uint64_t, std::uint64_t>>& RunsScratch()
		{
			static thread_local std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
			return runs;
		}

		void Touch()
		{
			if (!changed) {
				++contentEpoch;
				changed = true;
			}
		}

		std::uint64_t contentEpoch = 0;
		std::uint64_t floor = 0;
		bool changed = false;
		std::vector<Entry> entries;
		std::vector<std::uint64_t> markedAt;  // per element, the content epoch that last marked it
	};

	/**
	 * @brief Bytes per kept array chunk. A write after a view copies one chunk, not the array.
	 * 16 KB keeps a 1 KB material row's write at 16 rows' copy while the chunk table stays short.
	 */
	inline constexpr std::size_t kKeptChunkBytes = 16384;
	/** @brief The default chunk length: chunks of about kKeptChunkBytes. */
	inline constexpr std::size_t kKeptChunkBySize = 0;
	/**
	 * @brief One contiguous array instead, copied whole by a build's first write while a view shares it.
	 * For users that need one pointer over many elements, or write from many threads after one Mutable().
	 */
	inline constexpr std::size_t kKeptWhole = ~std::size_t(0);
	template <class T>
	constexpr std::size_t KeptChunkElements()
	{
		return std::bit_floor(std::max<std::size_t>(1, kKeptChunkBytes / sizeof(T)));
	}

	/** @brief Cumulative copy and reuse counts. A reporter diffs them between reports. */
	struct KeptCopyStats
	{
		std::uint64_t tableCopies = 0;       // the first write of a build a view shares the table with
		std::uint64_t chunkCopies = 0;       // the first write to a chunk a view holds
		std::uint64_t chunkReuses = 0;       // taken from chunks every view had released
		std::uint64_t chunkAllocations = 0;
		std::uint64_t bytesCopied = 0;
	};

	/** @brief How a KeptChunkStore tells that a view shares a chunk. */
	enum class KeptSharing : std::uint8_t
	{
		// By the chunk's use_count. A replaced chunk is retired until every view lets go, then reused.
		kByCount,
		// By a stamp that Freeze moves after each copy is taken. Only the writer decides sharing, never a count another thread
		// moves. Nothing is retired or reused, so a replaced chunk and the owners its elements hold go with their last holder.
		kByStamp,
	};

	template <class T, std::size_t kChunk, KeptSharing kSharing>
	class KeptChunkStore;

	/**
	 * @brief A kept array's elements as views see them: fixed power-of-two chunks shared with the array and other views, immutable once shared.
	 * Reads like a const std::vector without data(). Contiguous memory comes as chunk-bounded spans.
	 */
	template <class T>
	class KeptChunks
	{
	public:
		using value_type = T;
		using Chunk = std::vector<T>;

		class const_iterator
		{
		public:
			using iterator_category = std::random_access_iterator_tag;
			using value_type = T;
			using difference_type = std::ptrdiff_t;
			using pointer = const T*;
			using reference = const T&;

			const_iterator() = default;
			const_iterator(const KeptChunks* a_owner, std::size_t a_index) :
				owner(a_owner), index(a_index) {}
			reference operator*() const { return (*owner)[index]; }
			pointer operator->() const { return &(*owner)[index]; }
			reference operator[](difference_type a_n) const { return (*owner)[Moved(a_n)]; }
			const_iterator& operator++()
			{
				++index;
				return *this;
			}
			const_iterator operator++(int)
			{
				auto old = *this;
				++index;
				return old;
			}
			const_iterator& operator--()
			{
				--index;
				return *this;
			}
			const_iterator operator--(int)
			{
				auto old = *this;
				--index;
				return old;
			}
			const_iterator& operator+=(difference_type a_n)
			{
				index = Moved(a_n);
				return *this;
			}
			const_iterator& operator-=(difference_type a_n)
			{
				index = Moved(-a_n);
				return *this;
			}
			friend const_iterator operator+(const_iterator a_it, difference_type a_n) { return a_it += a_n; }
			friend const_iterator operator+(difference_type a_n, const_iterator a_it) { return a_it += a_n; }
			friend const_iterator operator-(const_iterator a_it, difference_type a_n) { return a_it -= a_n; }
			friend difference_type operator-(const const_iterator& a_left, const const_iterator& a_right)
			{
				return static_cast<difference_type>(a_left.index) - static_cast<difference_type>(a_right.index);
			}
			friend bool operator==(const const_iterator& a_left, const const_iterator& a_right) { return a_left.index == a_right.index; }
			friend auto operator<=>(const const_iterator& a_left, const const_iterator& a_right) { return a_left.index <=> a_right.index; }

		private:
			std::size_t Moved(difference_type a_n) const { return static_cast<std::size_t>(static_cast<difference_type>(index) + a_n); }

			const KeptChunks* owner = nullptr;
			std::size_t index = 0;
		};
		using iterator = const_iterator;

		KeptChunks() = default;
		explicit KeptChunks(std::uint32_t a_shift) :
			shift(a_shift) {}
		/** @brief Adopts elements of one's own as a single chunk, without copying. */
		static KeptChunks Adopt(std::shared_ptr<Chunk> a_flat)
		{
			KeptChunks out(63);
			if (a_flat) {
				out.count = a_flat->size();
				out.chunks.push_back(std::move(a_flat));
			}
			return out;
		}

		std::size_t size() const { return count; }
		bool empty() const { return !count; }
		const T& operator[](std::size_t a_index) const { return (*chunks[a_index >> shift])[a_index & Mask()]; }
		const T& back() const { return (*this)[count - 1]; }
		const_iterator begin() const { return { this, 0 }; }
		const_iterator end() const { return { this, count }; }
		std::size_t ChunkElements() const { return std::size_t{ 1 } << shift; }
		/** @brief Calls a_span(data, count, first) for [a_first, a_first + a_count), clipped, one chunk at a time. */
		template <class Span>
		void ForEachSpan(std::size_t a_first, std::size_t a_count, Span&& a_span) const
		{
			const std::size_t end = a_first + std::min(a_count, count > a_first ? count - a_first : 0);
			for (std::size_t at = a_first; at < end;) {
				const std::size_t offset = at & Mask();
				const std::size_t n = std::min(end - at, ChunkElements() - offset);
				a_span(static_cast<const T*>(chunks[at >> shift]->data() + offset), n, at);
				at += n;
			}
		}
		operator std::vector<T>() const
		{
			std::vector<T> out;
			out.reserve(count);
			ForEachSpan(0, count, [&](const T* a_data, std::size_t a_count, std::size_t) { out.insert(out.end(), a_data, a_data + a_count); });
			return out;
		}
		friend bool operator==(const KeptChunks& a_left, const std::vector<T>& a_right)
		{
			if (a_left.size() != a_right.size())
				return false;
			bool same = true;
			a_left.ForEachSpan(0, a_left.size(), [&](const T* a_data, std::size_t a_count, std::size_t a_at) {
				same = same && std::equal(a_data, a_data + a_count, a_right.begin() + static_cast<std::ptrdiff_t>(a_at));
			});
			return same;
		}
		friend bool operator==(const KeptChunks& a_left, const KeptChunks& a_right)
		{
			return a_left.size() == a_right.size() && std::equal(a_left.begin(), a_left.end(), a_right.begin());
		}

	private:
		template <class, std::size_t, KeptSharing>
		friend class KeptChunkStore;

		std::size_t Mask() const { return ChunkElements() - 1; }

		std::vector<std::shared_ptr<Chunk>> chunks;  // ceil(count / ChunkElements()) chunks. An adopted one is count long.
		std::size_t count = 0;
		std::uint32_t shift = 0;
	};

	/** @brief A view's shared, immutable handle on a kept array's elements. A plain vector is adopted as one chunk. */
	template <class T>
	class KeptElements
	{
	public:
		KeptElements() = default;
		KeptElements(std::nullptr_t) {}
		KeptElements(std::shared_ptr<const KeptChunks<T>> a_chunks) :
			chunks(std::move(a_chunks)) {}
		KeptElements(std::shared_ptr<std::vector<T>> a_flat) :
			chunks(a_flat ? std::shared_ptr<const KeptChunks<T>>(std::make_shared<KeptChunks<T>>(KeptChunks<T>::Adopt(std::move(a_flat)))) : nullptr) {}
		// Never written through. An adopted chunk belongs to no store.
		KeptElements(std::shared_ptr<const std::vector<T>> a_flat) :
			KeptElements(std::const_pointer_cast<std::vector<T>>(std::move(a_flat))) {}

		explicit operator bool() const { return chunks != nullptr; }
		const KeptChunks<T>& operator*() const { return *chunks; }
		const KeptChunks<T>* operator->() const { return chunks.get(); }
		const KeptChunks<T>* get() const { return chunks.get(); }
		void reset() { chunks.reset(); }

	private:
		std::shared_ptr<const KeptChunks<T>> chunks;
	};

	/** @brief Most bytes between two runs that are sent as one span. A 16-row gap of 16-byte rows costs less than another upload's bookkeeping. */
	inline constexpr std::size_t kKeptMergeGapBytes = 256;

	/** @brief A build's view of a kept array: the elements, shared copy-on-write by chunk, and their changes. */
	template <class T>
	struct KeptView
	{
		// Shared with the array, which copies a chunk before writing it while a view holds it. Never mutate the view.
		// Current-frame bindings belong in separate patch storage.
		KeptElements<T> elements;
		ChangeJournal::Snapshot changes;

		std::size_t Count() const { return elements ? elements->size() : 0; }
		const T* At(std::size_t a_index) const { return elements && a_index < elements->size() ? &(*elements)[a_index] : nullptr; }
		std::uint64_t ContentEpoch() const { return changes.contentEpoch; }
		/**
		 * @brief The elements a holder at a_contentEpoch lacks, as chunk-bounded spans. Runs in one chunk within kKeptMergeGapBytes merge.
		 * Nothing past a_capacity. Returns the elements.
		 */
		template <class Span>
		std::size_t ForEachSpan(std::uint64_t a_contentEpoch, Span&& a_span, std::size_t a_capacity = ~std::size_t(0)) const
		{
			return elements ? Spans(*elements, changes, a_contentEpoch, a_span, a_capacity) : 0;
		}
		/**
		 * @brief The uploads a holder at a_contentEpoch lacks, as a_emit(data, bytes, offset), cut at chunk ends. Returns the bytes sent.
		 * Nothing past a_capacity is sent. Those elements wait for the holder to grow.
		 */
		template <class Emit>
		std::size_t Emit(std::uint64_t a_contentEpoch, Emit&& a_emit, std::size_t a_capacity = ~std::size_t(0)) const
		{
			return elements ? EmitSpans(*elements, changes, a_contentEpoch, a_emit, a_capacity) : 0;
		}
		void Reset() { *this = {}; }

		/**
		 * @brief ForEachSpan over any elements and changes. Two runs in one chunk within kKeptMergeGapBytes go as one span.
		 * The holder already has the elements between them, so one upload ends the same as two.
		 */
		template <class Elements, class Changes, class Span>
		static std::size_t Spans(const Elements& a_elements, const Changes& a_changes, std::uint64_t a_contentEpoch, Span&& a_span, std::size_t a_capacity)
		{
			const std::size_t chunk = ChunkOf(a_elements);
			struct Pending
			{
				const T* data = nullptr;
				std::size_t count = 0, first = 0;
			} pending;
			std::size_t sent = 0;
			const auto flush = [&] {
				if (pending.count) {
					a_span(pending.data, pending.count, pending.first);
					sent += pending.count;
				}
			};
			// Runs come in order and apart, and each chunk's spans in order.
			const auto add = [&](const T* a_data, std::size_t a_count, std::size_t a_first) {
				if (pending.count && a_first / chunk == pending.first / chunk && (a_first - (pending.first + pending.count)) * sizeof(T) <= kKeptMergeGapBytes) {
					pending.count = a_first + a_count - pending.first;
					return;
				}
				flush();
				pending = { a_data, a_count, a_first };
			};
			a_changes.ForEachRun(a_contentEpoch, a_elements.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
				if (a_first >= a_capacity)
					return;
				a_count = std::min<std::uint64_t>(a_count, a_capacity - a_first);
				SpansOf(a_elements, static_cast<std::size_t>(a_first), static_cast<std::size_t>(a_count), add);
			});
			flush();
			return sent;
		}
		template <class Elements, class Changes, class EmitFn>
		static std::size_t EmitSpans(const Elements& a_elements, const Changes& a_changes, std::uint64_t a_contentEpoch, EmitFn&& a_emit, std::size_t a_capacity)
		{
			const auto emit = [&](const T* a_data, std::size_t a_count, std::size_t a_first) {
				a_emit(static_cast<const void*>(a_data), a_count * sizeof(T), a_first * sizeof(T));
			};
			return sizeof(T) * Spans(a_elements, a_changes, a_contentEpoch, emit, a_capacity);
		}

	private:
		static std::size_t ChunkOf(const KeptChunks<T>& a_elements) { return a_elements.ChunkElements(); }
		static std::size_t ChunkOf(const std::vector<T>&) { return ~std::size_t(0); }
		template <class Span>
		static void SpansOf(const KeptChunks<T>& a_elements, std::size_t a_first, std::size_t a_count, Span& a_span)
		{
			a_elements.ForEachSpan(a_first, a_count, a_span);
		}
		template <class Span>
		static void SpansOf(const std::vector<T>& a_elements, std::size_t a_first, std::size_t a_count, Span& a_span)
		{
			if (const std::size_t n = a_first < a_elements.size() ? std::min(a_count, a_elements.size() - a_first) : 0)
				a_span(a_elements.data() + a_first, n, a_first);
		}
	};

	/**
	 * @brief A kept array's own elements, used like a std::vector. A write first takes private copies of the chunk table and chunk if a view shares them,
	 * so views never change. With kByCount, replaced chunks are retired until every view releases them, then reused.
	 * With kByStamp, a copy of the store is a view: call Freeze after taking one. Writes then go through Mutable(index), and reads of a
	 * non-const store never copy.
	 * Single-threaded, except that after Prepare over a range, writes to distinct elements in it may come from many threads.
	 */
	template <class T, std::size_t kChunk, KeptSharing kSharing = KeptSharing::kByCount>
	class KeptChunkStore
	{
		static constexpr bool kByStamp = kSharing == KeptSharing::kByStamp;
		// Functions, not constants, because a kept array may be declared while T is incomplete.
		static constexpr std::size_t Length()
		{
			constexpr std::size_t length = kChunk == kKeptChunkBySize ? KeptChunkElements<T>() : kChunk;
			static_assert(std::has_single_bit(length));
			return length;
		}
		static constexpr std::uint32_t Shift() { return static_cast<std::uint32_t>(std::countr_zero(Length())); }
		static constexpr std::size_t Mask() { return Length() - 1; }
		using Chunk = std::vector<T>;

	public:
		KeptChunkStore() = default;
		// A copy shares the elements but not the chunks kept for reuse, which each store owns.
		// With kByStamp it holds every chunk as shared, and keeps the counts.
		KeptChunkStore(const KeptChunkStore& a_other) :
			table(a_other.table)
		{
			if constexpr (kByStamp)
				CopyStamps(a_other);
		}
		KeptChunkStore& operator=(const KeptChunkStore& a_other)
		{
			if (this != &a_other) {
				table = a_other.table;
				free.clear();
				retired.clear();
				if constexpr (kByStamp)
					CopyStamps(a_other);
			}
			return *this;
		}
		KeptChunkStore(KeptChunkStore&&) noexcept = default;
		KeptChunkStore& operator=(KeptChunkStore&&) noexcept = default;

		std::size_t size() const { return table->count; }
		bool empty() const { return !table->count; }
		const T& operator[](std::size_t a_index) const { return (*table)[a_index]; }
		T& operator[](std::size_t a_index)
			requires(!kByStamp)
		{
			return Mutable(a_index);
		}
		const T& back() const { return (*table)[table->count - 1]; }
		T& back()
			requires(!kByStamp)
		{
			return Mutable(table->count - 1);
		}
		const KeptChunks<T>& Get() const { return *table; }
		KeptChunkStore& Mutable() { return *this; }
		/** @brief Element a_index for writing, its chunk copied first if a view shares it. */
		T& Mutable(std::size_t a_index) { return Own(a_index >> Shift())[a_index & Mask()]; }
		/** @brief kByStamp, after a copy was taken: every chunk and the table are shared from here on. */
		void Freeze()
			requires(kByStamp)
		{
			++stamp;
		}
		/** @brief kByStamp: every write so far, a copy's included. Equal while nothing was written. */
		std::uint64_t Writes() const
			requires(kByStamp)
		{
			return writes;
		}

		void resize(std::size_t a_count) { resize(a_count, T{}); }
		void resize(std::size_t a_count, const T& a_value)
		{
			OwnTable();
			auto& t = *table;
			if constexpr (kByStamp)
				++writes;
			if (a_count <= t.count) {
				const std::size_t keep = (a_count + Mask()) >> Shift();
				// kByStamp resets the dropped tail of the last kept chunk, so the owners its elements hold go now.
				if constexpr (kByStamp) {
					if (const std::size_t used = a_count & Mask(); used && a_count < t.count) {
						auto& tail = Own(a_count >> Shift());
						std::fill(tail.begin() + static_cast<std::ptrdiff_t>(used), tail.begin() + static_cast<std::ptrdiff_t>(std::min(Length(), used + (t.count - a_count))), T{});
					}
				}
				while (t.chunks.size() > keep) {
					Drop(std::move(t.chunks.back()));
					PopChunk();
				}
				t.count = a_count;
				return;
			}
			// The rest of the last chunk, which an earlier shrink left as it was, then whole chunks.
			if (const std::size_t used = t.count & Mask()) {
				auto& tail = Own(t.count >> Shift());
				std::fill(tail.begin() + static_cast<std::ptrdiff_t>(used), tail.begin() + static_cast<std::ptrdiff_t>(std::min(Length(), used + (a_count - t.count))), a_value);
			}
			const std::size_t need = (a_count + Mask()) >> Shift();
			while (t.chunks.size() < need) {
				auto chunk = Take();
				chunk->assign(Length(), a_value);
				PushChunk(std::move(chunk));
			}
			peakChunks = std::max(peakChunks, t.chunks.size());
			t.count = a_count;
		}
		void push_back(const T& a_value)
		{
			OwnTable();
			auto& t = *table;
			if (!(t.count & Mask())) {
				auto chunk = Take();
				chunk->resize(Length());
				PushChunk(std::move(chunk));
				peakChunks = std::max(peakChunks, t.chunks.size());
			}
			Own(t.count >> Shift())[t.count & Mask()] = a_value;
			++t.count;
		}
		void pop_back()
		{
			OwnTable();
			auto& t = *table;
			if constexpr (kByStamp)
				++writes;
			if (!(--t.count & Mask())) {
				Drop(std::move(t.chunks.back()));
				PopChunk();
			}
		}
		void clear() { resize(0); }

		/** @brief Takes the table and every chunk of the range for this store, so writes there become concurrent-safe. */
		void Prepare(std::size_t a_first, std::size_t a_count)
		{
			OwnTable();
			const std::size_t end = a_first + std::min(a_count, size() > a_first ? size() - a_first : 0);
			if (a_first >= end)
				return;
			for (std::size_t c = a_first >> Shift(); c <= (end - 1) >> Shift(); ++c)
				Own(c);
		}
		/** @brief Reuses chunks retired while views held them, once none does. */
		void BeginBuild()
		{
			for (std::size_t i = 0; i < retired.size();) {
				if (retired[i].use_count() != 1) {
					++i;
					continue;
				}
				if (free.size() < peakChunks)
					free.push_back(std::move(retired[i]));
				retired[i] = std::move(retired.back());
				retired.pop_back();
			}
			// The views' last reads of reused chunks happen before the next writes.
			std::atomic_thread_fence(std::memory_order_acquire);
		}
		std::shared_ptr<const KeptChunks<T>> Share() const { return table; }
		const KeptCopyStats& Stats() const { return stats; }

	private:
		/** @brief kByStamp: a copy holds every chunk and the table as shared, and keeps the counts. */
		void CopyStamps(const KeptChunkStore& a_other)
		{
			stamps.assign(a_other.stamps.size(), 0);
			tableStamp = 0;
			stamp = 1;
			writes = a_other.writes;
			stats = a_other.stats;
		}
		void PushChunk(std::shared_ptr<Chunk>&& a_chunk)
		{
			table->chunks.push_back(std::move(a_chunk));
			if constexpr (kByStamp)
				stamps.push_back(stamp);
		}
		void PopChunk()
		{
			table->chunks.pop_back();
			if constexpr (kByStamp)
				stamps.pop_back();
		}
		/** @brief The chunk table, copied first if a view shares it. */
		void OwnTable()
		{
			if constexpr (kByStamp) {
				if (tableStamp == stamp) [[likely]]
					return;
				tableStamp = stamp;
			} else if (table.use_count() == 1) [[likely]] {
				return;
			}
			table = std::make_shared<KeptChunks<T>>(*table);
			++stats.tableCopies;
		}
		/** @brief Chunk a_chunk for writing, copied first if a view holds it. */
		Chunk& Own(std::size_t a_chunk)
		{
			OwnTable();
			auto& chunk = table->chunks[a_chunk];
			if constexpr (kByStamp) {
				++writes;
				if (stamps[a_chunk] != stamp) {
					chunk = std::make_shared<Chunk>(*chunk);
					stamps[a_chunk] = stamp;
					++stats.chunkCopies;
					stats.bytesCopied += Length() * sizeof(T);
				}
				return *chunk;
			}
			if (chunk.use_count() == 1) [[likely]] {
				// A view that just let go read it before this write.
				std::atomic_thread_fence(std::memory_order_acquire);
				return *chunk;
			}
			auto next = Take();
			*next = *chunk;
			++stats.chunkCopies;
			stats.bytesCopied += Length() * sizeof(T);
			Drop(std::move(chunk));
			chunk = std::move(next);
			return *chunk;
		}
		/** @brief A chunk of this store's own, reused or new. The caller sets its contents. */
		std::shared_ptr<Chunk> Take()
		{
			if (!kByStamp && reuseKeptStorage && !free.empty()) {
				auto chunk = std::move(free.back());
				free.pop_back();
				++stats.chunkReuses;
				return chunk;
			}
			++stats.chunkAllocations;
			return std::make_shared<Chunk>();
		}
		/** @brief Gives up a chunk. Reusable now if no view holds it, otherwise at BeginBuild once none does. */
		void Drop(std::shared_ptr<Chunk>&& a_chunk)
		{
			if (kByStamp || !reuseKeptStorage || !a_chunk)
				return;
			if (a_chunk.use_count() != 1)
				retired.push_back(std::move(a_chunk));
			else if (free.size() < peakChunks)
				free.push_back(std::move(a_chunk));
		}

		std::shared_ptr<KeptChunks<T>> table = std::make_shared<KeptChunks<T>>(Shift());
		std::vector<std::shared_ptr<Chunk>> free;     // no view holds them, so reused first
		std::vector<std::shared_ptr<Chunk>> retired;  // replaced while a view held them. Freed here, never on a view's thread.
		std::size_t peakChunks = 0;                   // the most the table had, which sets what is kept for reuse
		KeptCopyStats stats;
		// kByStamp only. Each chunk's stamp and the table's: a part is shared when its stamp is not the current one.
		std::vector<std::uint64_t> stamps;  // by chunk
		std::uint64_t tableStamp = 1, stamp = 1;
		std::uint64_t writes = 0;
	};

	/**
	 * @brief A kept array's elements as one contiguous std::vector. A build's first Mutable() copies it whole into a released spare if a view holds it.
	 * After that, writes to distinct elements may come from many threads. Views see it as one chunk.
	 */
	template <class T>
	class KeptWholeStore
	{
	public:
		void BeginBuild() { copied = false; }
		const std::vector<T>& Get() const { return *elements; }
		std::size_t size() const { return elements->size(); }
		const T& operator[](std::size_t a_index) const { return (*elements)[a_index]; }
		T& operator[](std::size_t a_index) { return Mutable()[a_index]; }
		std::vector<T>& Mutable()
		{
			if (copied)
				return *elements;
			copied = true;
			if (elements.use_count() == 1)
				return *elements;
			++stats.chunkCopies;
			stats.bytesCopied += elements->size() * sizeof(T);
			if (!reuseKeptStorage) {
				++stats.chunkAllocations;
				elements = std::make_shared<std::vector<T>>(*elements);
				return *elements;
			}
			// Reuse storage only after every view released it, and copy the whole array so a build never writes an outstanding view.
			std::shared_ptr<std::vector<T>> next;
			for (auto it = spare.begin(); it != spare.end(); ++it) {
				if (it->use_count() == 1) {
					next = std::move(*it);
					spare.erase(it);
					break;
				}
			}
			if (next) {
				std::atomic_thread_fence(std::memory_order_acquire);
				++stats.chunkReuses;
			} else {
				++stats.chunkAllocations;
				next = std::make_shared<std::vector<T>>();
			}
			*next = *elements;
			if (spare.size() < 6)  // views live across frames, because publications hold their payloads
				spare.push_back(std::move(elements));
			elements = std::move(next);
			return *elements;
		}
		void Prepare(std::size_t, std::size_t) { Mutable(); }
		void clear() { Mutable().clear(); }
		std::shared_ptr<const KeptChunks<T>> Share() const { return std::make_shared<KeptChunks<T>>(KeptChunks<T>::Adopt(elements)); }
		const KeptCopyStats& Stats() const { return stats; }

	private:
		std::shared_ptr<std::vector<T>> elements = std::make_shared<std::vector<T>>();
		std::vector<std::shared_ptr<std::vector<T>>> spare;
		bool copied = false;
		KeptCopyStats stats;
	};

	/**
	 * @brief An array kept across builds and mirrored by GPU buffers. Copy-on-write by chunk against the views payloads hold, and journalled
	 * so each buffer is sent only what changed since its content epoch.
	 * The journal is single-threaded. For parallel writes: PrepareWrites on the owner thread, Write or WriteParts to distinct elements from any thread,
	 * then Mark or MarkParts on the owner thread in a deterministic order.
	 */
	template <class T, std::size_t kChunk = kKeptChunkBySize>
	class KeptArray
	{
	public:
		using Elements = std::conditional_t<kChunk == kKeptWhole, KeptWholeStore<T>, KeptChunkStore<T, kChunk>>;

		/** @brief A build's start. Released chunks become reusable again. */
		void BeginBuild(std::uint64_t a_oldestContentEpoch)
		{
			journal.BeginBuild(a_oldestContentEpoch);
			elements.BeginBuild();
		}
		decltype(auto) Get() const { return elements.Get(); }
		std::size_t Size() const { return elements.size(); }
		/** @brief The elements to change. Mark what changes. Chunks are copied as first written while a view holds them. */
		decltype(auto) Mutable() { return elements.Mutable(); }
		/** @brief Takes the range's chunks for writing up front, so writes to distinct elements are safe from many threads until the next single-threaded call. */
		void PrepareWrites(std::size_t a_first, std::size_t a_count) { elements.Prepare(a_first, a_count); }
		/** @brief Writes an element only if its bytes differ, unmarked. Returns whether it did. Concurrent-safe in a prepared range. */
		bool Write(std::size_t a_index, const T& a_value)
		{
			if (std::memcmp(&std::as_const(elements)[a_index], &a_value, sizeof(T)) == 0)
				return false;
			elements[a_index] = a_value;
			return true;
		}
		/** @brief Writes an element when any of its a_parts equal slices differ, unmarked. Returns the mask of differing parts. Concurrent-safe in a prepared range. */
		std::uint64_t WriteParts(std::size_t a_index, const T& a_value, std::size_t a_parts)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const std::size_t partBytes = sizeof(T) / a_parts;
			const auto* held = reinterpret_cast<const std::byte*>(&std::as_const(elements)[a_index]);
			const auto* value = reinterpret_cast<const std::byte*>(&a_value);
			std::uint64_t mask = 0;
			for (std::size_t p = 0; p < a_parts; ++p)
				if (std::memcmp(held + p * partBytes, value + p * partBytes, partBytes) != 0)
					mask |= std::uint64_t{ 1 } << p;
			if (mask)
				elements[a_index] = a_value;
			return mask;
		}
		/** @brief Marks element a_index's parts in a_mask. Part p is a_index * a_parts + p in the journal. */
		void MarkParts(std::size_t a_index, std::uint64_t a_mask, std::size_t a_parts)
		{
			for (; a_mask; a_mask &= a_mask - 1)
				journal.Mark(a_index * a_parts + static_cast<std::size_t>(std::countr_zero(a_mask)));
		}
		/** @brief Writes and marks an element only if its bytes differ. Returns whether it did. */
		bool Set(std::size_t a_index, const T& a_value)
		{
			if (!Write(a_index, a_value))
				return false;
			journal.Mark(a_index);
			return true;
		}
		/** @brief Writes an element and marks only the parts that differ. Such an array's journal and views are by part. Returns the parts marked. */
		std::size_t SetParts(std::size_t a_index, const T& a_value, std::size_t a_parts)
		{
			const std::uint64_t mask = WriteParts(a_index, a_value, a_parts);
			MarkParts(a_index, mask, a_parts);
			return static_cast<std::size_t>(std::popcount(mask));
		}
		void Mark(std::size_t a_index) { journal.Mark(a_index); }
		void MarkRange(std::uint64_t a_first, std::uint64_t a_count) { journal.MarkRange(a_first, a_count); }
		void Resync() { journal.Resync(); }
		/** @brief Empties the array and sends every holder everything again. */
		void Clear()
		{
			elements.clear();
			journal.Resync();
		}
		std::uint64_t ContentEpoch() const { return journal.ContentEpoch(); }
		bool Changed() const { return journal.Changed(); }
		KeptView<T> View() const { return { KeptElements<T>(elements.Share()), journal.Take() }; }
		/** @brief View().Emit without copying anything, straight from the live journal. Only valid between builds. */
		template <class EmitFn>
		std::size_t Emit(std::uint64_t a_contentEpoch, EmitFn&& a_emit, std::size_t a_capacity = ~std::size_t(0)) const
		{
			return KeptView<T>::EmitSpans(elements.Get(), journal, a_contentEpoch, a_emit, a_capacity);
		}
		/** @brief The elements themselves, for a non-journalled view sent whole. */
		KeptElements<T> Shared() const { return KeptElements<T>(elements.Share()); }
		const KeptCopyStats& CopyStats() const { return elements.Stats(); }

	private:
		Elements elements;
		ChangeJournal journal;
	};
	template <class T>
	using KeptWholeArray = KeptArray<T, kKeptWhole>;

	/** @brief A list of indices with a mark per index, so each is listed once. */
	struct MarkedList
	{
		std::vector<std::uint32_t> list;
		std::vector<std::uint8_t> mark;

		/** @brief Lists a_index. Returns false if it was already listed. */
		bool Add(std::uint32_t a_index)
		{
			if (mark.size() <= a_index)
				mark.resize(std::size_t(a_index) + 1, 0);
			if (mark[a_index])
				return false;
			mark[a_index] = 1;
			list.push_back(a_index);
			return true;
		}
		bool Contains(std::uint32_t a_index) const { return a_index < mark.size() && mark[a_index]; }
		/** @brief Unlists the element at list position a_at. The last one takes its place. */
		void RemoveAt(std::size_t a_at)
		{
			mark[list[a_at]] = 0;
			list[a_at] = list.back();
			list.pop_back();
		}
		void Clear()
		{
			for (const auto index : list)
				mark[index] = 0;
			list.clear();
		}
		std::size_t Size() const { return list.size(); }
		bool Empty() const { return list.empty(); }
	};
}

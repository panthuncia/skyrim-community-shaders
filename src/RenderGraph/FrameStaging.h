#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <atomic>
#	include <cstdint>
#	include <memory>
#	include <mutex>
#	include <string>
#	include <vector>

namespace org
{
	class LatchBlock;
}

namespace RenderGraph
{
	/**
	 * @brief Host-visible staging buffers for a client's per-run CPU data (vertices, indices, constants, decoded pixels) that its pass
	 * copies from or reads, reused instead of made and freed every run.
	 *
	 * Acquire hands out a one-slot LatchBlock of at least the size asked for. It comes back to the pool when its last holder lets it
	 * go, and the holder that lets it go last is the recording that used it (PassRecordContext::Retain, or a frame object the
	 * recording retains): so it returns once the GPU is done with it, never earlier. The pool keeps up to kGameFramesInFlight times
	 * runsPerFrame spare blocks (plus one being filled), which is how many are in flight when the anchor runs that often: a steady
	 * frame makes no block. A pool that had to make more than it keeps logs it once (raise runsPerFrame, or use the anchor's
	 * FrameAnchors::RunsPerFrame).
	 *
	 * Sizes: a new block is the next power of two at or above max(bytes asked, minimumBytes); Acquire takes the smallest spare
	 * block that fits. With maxBlocks set, Acquire returns null once that many exist and none is spare: the caller keeps the
	 * engine's path for that run (Bink's decoded frames: a few blocks of one large size).
	 *
	 * Threads: Acquire on the render thread (a commit). Blocks come back on any thread. The pool may be destroyed with blocks still
	 * out: they are then released when their holders let them go.
	 *
	 * Usage: `auto staging = RenderGraph::FrameStaging::Create({ .name = "cs.scaleform.staging", .minimumBytes = 256u << 10,
	 * .runsPerFrame = FrameAnchors::RunsPerFrame(FrameAnchors::Anchor::Interface) });` then, in the commit,
	 * `frame->staging = staging->Acquire(bytes);` and in Record `a_recording.Retain(frame);`.
	 */
	class FrameStaging : public std::enable_shared_from_this<FrameStaging>
	{
	public:
		struct Desc
		{
			std::string name;                     // the blocks' debug name
			std::uint32_t minimumBytes = 64u << 10;  // the smallest block made
			std::uint32_t runsPerFrame = 1;       // the client's runs a frame, each holding one block (spares kept: kGameFramesInFlight times this, plus one)
			std::uint32_t maxBlocks = 0;          // 0: no limit
		};

		/** @brief A pool. Any thread. */
		static std::shared_ptr<FrameStaging> Create(Desc a_desc);

		/** @brief A block of at least a_bytes, given back when its last holder lets it go; null when it cannot be made or maxBlocks are out. */
		std::shared_ptr<org::LatchBlock> Acquire(std::uint64_t a_bytes);

		/** @brief Blocks that exist now, out or spare. */
		std::uint32_t Made() const { return made.load(std::memory_order_relaxed); }

		/** @brief Blocks in the pool now. */
		std::uint32_t Spare() const;

		explicit FrameStaging(Desc a_desc);

	private:
		void GiveBack(std::shared_ptr<org::LatchBlock> a_block);

		Desc desc;
		std::uint32_t keep = 0;  // the most spare blocks kept
		mutable std::mutex mutex;
		std::vector<std::shared_ptr<org::LatchBlock>> spare;
		std::atomic<std::uint32_t> made{ 0 };
		std::atomic_flag churnLogged;
	};

	/**
	 * @brief One value per host frame slot, handed from a client's commit to the pass that records that slot: the commit Stores at
	 * the host's CurrentFrameSlot, the pass Takes at its PassRecordContext::FrameSlot. A slot left unread is replaced by the next
	 * Store there; Clear empties every slot (a pass of an older build that still holds this object then records nothing).
	 * Thread-safe (each slot is an atomic shared_ptr).
	 */
	template <class T>
	class RunSlots
	{
	public:
		explicit RunSlots(std::uint32_t a_count) :
			values(std::make_unique<std::atomic<std::shared_ptr<const T>>[]>(a_count ? a_count : 1)), count(a_count ? a_count : 1) {}

		void Store(std::uint32_t a_slot, std::shared_ptr<const T> a_value) const { values[a_slot % count].store(std::move(a_value), std::memory_order_release); }
		std::shared_ptr<const T> Take(std::uint32_t a_slot) const { return values[a_slot % count].exchange(nullptr, std::memory_order_acq_rel); }

		void Clear() const
		{
			for (std::uint32_t i = 0; i < count; ++i)
				values[i].store(nullptr, std::memory_order_release);
		}

		/** @brief The slots it holds: the host's FrameSlots when it was made. */
		std::uint32_t Count() const { return count; }

	private:
		std::unique_ptr<std::atomic<std::shared_ptr<const T>>[]> values;
		std::uint32_t count;
	};
}

#endif

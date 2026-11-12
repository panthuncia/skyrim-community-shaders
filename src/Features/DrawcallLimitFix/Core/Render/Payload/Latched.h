#pragma once

// Latched copies: the per-frame values a commit writes into a latch block and its epoch's first pass copies to their targets.
// Self-contained (features include it); Internal.h includes it in its order.

#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"

#include <Render/LatchBlock.h>
#include <Resources/Buffers/Buffer.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace DCLF::Draws
{
	/**
	 * @brief A copy a main epoch's latched-copies pass (LatchedCopiesPass) records: bytes of the latch block's region of the run, at
	 * latchOffset, to a target the pass declared, at dstOffset. The commit wrote the bytes (LatchedUploads).
	 */
	struct LatchedCopy
	{
		const void* target = nullptr;  // LatchedTarget::key: the buffer, or a versioned buffer's (VersionedBuffer::Key)
		std::uint32_t latchOffset = 0, bytes = 0;
		std::uint64_t dstOffset = 0;
		bool operator==(const LatchedCopy&) const = default;
	};
	/**
	 * @brief A commit's latched copies: the block they read, and the list (part of the frame's shape). fixed: a dedicated block whose one
	 * region every run reads (the counters' zeros), else the run's region.
	 */
	struct LatchedList
	{
		std::shared_ptr<const org::LatchBlock> latch;
		std::vector<LatchedCopy> copies;
		bool fixed = false;
		bool operator==(const LatchedList& o) const { return latch == o.latch && copies == o.copies && fixed == o.fixed; }
	};
	/**
	 * @brief Which parts of each run's latch region already hold the bytes a snapshot fixes (its revision's bucket tables and maps, the
	 * shadow views' map rows). A published copy's region is pinned to its lease index (org::RunRef::Stable) and only its commit writes
	 * those places, so bytes keyed to the same snapshot are written once per copy, and again only when the key moves (a new latch block,
	 * revision or source). A leased run's region is new for each run (an external run's), so nothing of it is held before its commit
	 * writes it. The per-frame values (camera, counts, stamps, FrameValues' indices) are written by every commit. Render thread.
	 */
	struct LatchRegionsHeld
	{
		struct Slot
		{
			org::RunRef run{};                        // the run whose region the parts were written to
			std::weak_ptr<const void> latch, source;  // owner identities (no address reuse while held)
			std::uint64_t extra = 0;                  // what else the bytes depend on (the installed pipeline slots)
			std::vector<std::uint8_t> parts;          // by the caller's part index: written since the key was set
		};
		/** @brief a_run's record, emptied (no part held) when it is another run's, or its key is not (a_latch, a_source, a_extra). */
		Slot& Of(const org::RunRef& a_run, const std::shared_ptr<const void>& a_latch, const std::shared_ptr<const void>& a_source, std::uint64_t a_extra)
		{
			if (a_run.lease >= slots.size())
				slots.resize(std::size_t(a_run.lease) + 1);
			auto& slot = slots[a_run.lease];
			if (!(slot.run == a_run) || !Same(slot.latch, a_latch) || !Same(slot.source, a_source) || slot.extra != a_extra) {
				slot.run = a_run;
				slot.latch = a_latch;
				slot.source = a_source;
				slot.extra = a_extra;
				slot.parts.clear();
			}
			return slot;
		}
		/** @brief Whether part a_part of a_slot is held; marks it held (the caller writes it when it was not). */
		static bool Hold(Slot& a_slot, std::size_t a_part)
		{
			if (a_part >= a_slot.parts.size())
				a_slot.parts.resize(a_part + 1, 0);
			return std::exchange(a_slot.parts[a_part], std::uint8_t{ 1 }) != 0;
		}
		static bool Same(const std::weak_ptr<const void>& a_held, const std::shared_ptr<const void>& a_now)
		{
			return a_now && !a_held.owner_before(a_now) && !a_now.owner_before(a_held);
		}

		std::vector<Slot> slots;  // by lease index
	};

	/**
	 * @brief A shader's latch words for a_run (Record: PassRecordContext::Run): latchIndex the view of the arena chunk the run's region
	 * is in, latchOffset the region's offset there plus a_offset. Both depend on the run, so a pass sets them where it records.
	 */
	template <class Constants>
	void SetLatchWords(Constants& a_constants, const org::LatchBlock& a_latch, const org::RunRef& a_run, std::uint64_t a_offset = 0)
	{
		const auto region = a_latch.At(a_run);
		a_constants.latchIndex = region.srvIndex;
		a_constants.latchOffset = static_cast<std::uint32_t>(region.offset + a_offset);
	}

	/** @brief A target a latched copy may write: a buffer, or a versioned buffer (copied into the version its preparation resolved). */
	struct LatchedTarget
	{
		std::shared_ptr<org::Buffer> buffer;
		Versioned versioned;
		const void* Key() const { return versioned ? versioned->Key() : buffer.get(); }
	};
}

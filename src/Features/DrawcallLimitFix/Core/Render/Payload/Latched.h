#pragma once

// Latched copies. A commit writes per-frame values into a latch block and its epoch's first pass copies them to their targets.
// Features include this header directly, so it must stay self-contained.

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
	/** @brief One copy from the run's latch region at latchOffset to a declared target at dstOffset. */
	struct LatchedCopy
	{
		const void* target = nullptr;  // LatchedTarget::key: the buffer, or a versioned buffer's (VersionedBuffer::Key)
		std::uint32_t latchOffset = 0, bytes = 0;
		std::uint64_t dstOffset = 0;
		bool operator==(const LatchedCopy&) const = default;
	};
	/** @brief A commit's latched copies. A fixed list uses a dedicated block whose one region every run reads. */
	struct LatchedList
	{
		std::shared_ptr<const org::LatchBlock> latch;
		std::vector<LatchedCopy> copies;
		bool fixed = false;
		bool operator==(const LatchedList& o) const { return latch == o.latch && copies == o.copies && fixed == o.fixed; }
	};
	/**
	 * @brief Tracks which parts of each run's latch region already hold snapshot-fixed bytes. Render thread only.
	 * A published copy's region is pinned to its lease, so those bytes are rewritten only when the key changes.
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
		/** @brief a_run's record, cleared when it belongs to another run or another key. */
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
		/** @brief Marks part a_part held and returns whether it already was. */
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

	/** @brief Sets a shader's latch words for a_run. Both depend on the run, so set them at record time. */
	template <class Constants>
	void SetLatchWords(Constants& a_constants, const org::LatchBlock& a_latch, const org::RunRef& a_run, std::uint64_t a_offset = 0)
	{
		const auto region = a_latch.At(a_run);
		a_constants.latchIndex = region.srvIndex;
		a_constants.latchOffset = static_cast<std::uint32_t>(region.offset + a_offset);
	}

	struct LatchedTarget
	{
		std::shared_ptr<org::Buffer> buffer;
		Versioned versioned;
		const void* Key() const { return versioned ? versioned->Key() : buffer.get(); }
	};
}

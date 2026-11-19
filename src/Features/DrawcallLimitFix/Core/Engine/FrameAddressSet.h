#pragma once

// A lock-free set of engine object addresses that lasts one round. A geometry's first main registration in a round takes DCLF's
// shadow light bits, and later ones read the cleared mask, matching the engine's once-a-frame rule. One per light source.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace DCLF
{
	class FrameAddressSet
	{
	public:
		/** @brief Render thread, while no registration reads the set. Starts an empty round. Slots are cleared when the 16-bit round wraps. */
		void NewRound()
		{
			round = (round + 1) & 0xFFFF;
			if (!slots || round == 0) {
				if (!slots)
					slots = std::make_unique<std::atomic<std::uint64_t>[]>(kSlots);
				for (std::size_t i = 0; i < kSlots; ++i)
					slots[i].store(0, std::memory_order_relaxed);
				round += round == 0 ? 1 : 0;
			}
			tag.store(static_cast<std::uint64_t>(round) << 48, std::memory_order_release);
		}

		enum class Found
		{
			kNo,  // not in this round's set, until inserted
			kYes,
			kFull,  // no free slot within the probe, so the caller writes its bits again
		};

		/** @brief Any thread. Whether a_address is in this round's set, inserting it if asked. */
		Found Test(const void* a_address, bool a_insert)
		{
			const std::uint64_t key = reinterpret_cast<std::uintptr_t>(a_address) & kAddressMask;
			const std::uint64_t now = tag.load(std::memory_order_acquire);
			std::size_t slot = static_cast<std::size_t>(((key >> 4) * 0x9E3779B97F4A7C15ull) >> 48) & (kSlots - 1);
			for (std::uint32_t probe = 0; probe < kProbe; ++probe, slot = (slot + 1) & (kSlots - 1)) {
				auto& entry = slots[slot];
				std::uint64_t value = entry.load(std::memory_order_acquire);
				// Another round's slot is free, so the probe ends there.
				while ((value & ~kAddressMask) != now) {
					if (!a_insert)
						return Found::kNo;
					if (entry.compare_exchange_weak(value, now | key, std::memory_order_acq_rel, std::memory_order_acquire))
						return Found::kNo;
				}
				if ((value & kAddressMask) == key)
					return Found::kYes;
			}
			return Found::kFull;
		}

		/** @brief Whether NewRound has made the slots. */
		bool Ready() const { return slots != nullptr; }

	private:
		static constexpr std::size_t kSlots = std::size_t{ 1 } << 16;
		static constexpr std::uint32_t kProbe = 64;
		static constexpr std::uint64_t kAddressMask = 0x0000FFFFFFFFFFFFull;
		// (round << 48) | address. A slot of another round is free.
		std::unique_ptr<std::atomic<std::uint64_t>[]> slots;
		std::atomic<std::uint64_t> tag{ 0 };
		std::uint32_t round = 0;  // render thread
	};
}

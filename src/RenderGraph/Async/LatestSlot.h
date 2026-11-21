#pragma once

#include <atomic>
#include <memory>

namespace RenderGraph::Frame
{
	/**
	 * @brief A lock-free latest-wins mailbox: Post replaces what is there, Take empties it. MSVC's std::atomic<std::shared_ptr> is not lock-free.
	 * A value nobody took is freed by the next Post or the slot's end, on that thread.
	 * So never post a value here bare if it must be dropped on a given thread, such as an engine reference.
	 */
	template <class T>
	struct LatestSlot
	{
		std::atomic<T*> slot{ nullptr };

		LatestSlot() = default;
		LatestSlot(const LatestSlot&) = delete;
		LatestSlot& operator=(const LatestSlot&) = delete;
		~LatestSlot() { delete slot.exchange(nullptr, std::memory_order_acquire); }

		void Post(std::unique_ptr<T> a_value) { delete slot.exchange(a_value.release(), std::memory_order_acq_rel); }
		std::unique_ptr<T> Take() { return std::unique_ptr<T>(slot.exchange(nullptr, std::memory_order_acq_rel)); }
		/** @brief Takes the newest into a_held if there is one. */
		bool TakeInto(T& a_held)
		{
			if (auto taken = Take()) {
				a_held = std::move(*taken);
				return true;
			}
			return false;
		}
	};
}

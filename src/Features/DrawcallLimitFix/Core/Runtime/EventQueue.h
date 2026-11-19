#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace DCLF
{
	/**
	 * @brief Events the engine's hooks push from any thread, for one consumer to drain.
	 * A bounded Vyukov ring, so a push allocates nothing while there is room. When full, a push spills onto a lock-free stack.
	 * No event is ever lost, because consumers have no way to rebuild what a lost event would have told them.
	 * A drain visits the ring's events in push order, then the spilled ones in push order.
	 */
	template <class T, std::size_t Capacity = 4096>
	class EventQueue
	{
		static_assert((Capacity & (Capacity - 1)) == 0, "a power of two");

	public:
		EventQueue()
		{
			for (std::size_t i = 0; i < Capacity; ++i)
				cells[i].sequence.store(i, std::memory_order_relaxed);
		}
		/** @brief a_onPush runs after every push, on the pushing thread, to wake the consumer. */
		explicit EventQueue(void (*a_onPush)()) :
			EventQueue()
		{
			onPush = a_onPush;
		}
		// The queues live as long as the process and are never cleaned up at its end,
		// because nothing may release engine objects while the process is torn down.
		~EventQueue() = default;
		EventQueue(const EventQueue&) = delete;
		EventQueue& operator=(const EventQueue&) = delete;

		void Push(T a_value)
		{
			PushOnly(std::move(a_value));
			if (onPush)
				onPush();
		}

		/**
		 * @brief The one consumer. Visits every event pushed before the call, oldest first, and returns how many.
		 * Bounded by the ring's head at the call, so a drain always ends even while producers keep pushing.
		 */
		template <class F>
		std::size_t Drain(F&& a_visit)
		{
			std::size_t count = 0;
			const std::uint64_t bound = head.load(std::memory_order_acquire);
			for (; tail != bound;) {
				auto& cell = cells[tail & (Capacity - 1)];
				if (cell.sequence.load(std::memory_order_acquire) != tail + 1)
					break;
				a_visit(std::move(cell.value));
				cell.value = T{};
				cell.sequence.store(tail + Capacity, std::memory_order_release);
				++tail;
				++count;
			}
			// The spilled events, reversed into push order.
			Spill* reversed = nullptr;
			for (auto* node = spill.exchange(nullptr, std::memory_order_acquire); node;) {
				auto* next = node->next;
				node->next = reversed;
				reversed = node;
				node = next;
			}
			for (auto* node = reversed; node;) {
				a_visit(std::move(node->value));
				auto* next = node->next;
				delete node;
				node = next;
				++count;
			}
			return count;
		}
		/** @brief Drops every event pushed before the call. */
		void Discard() { Drain([](T&&) {}); }
		/** @brief Pushes that found the ring full since the start. */
		std::uint64_t Spilled() const { return spilled.load(std::memory_order_relaxed); }

	private:
		void PushOnly(T a_value)
		{
			std::uint64_t position = head.load(std::memory_order_relaxed);
			for (;;) {
				auto& cell = cells[position & (Capacity - 1)];
				const std::uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
				const auto difference = static_cast<std::int64_t>(sequence) - static_cast<std::int64_t>(position);
				if (difference == 0) {
					if (head.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
						cell.value = std::move(a_value);
						cell.sequence.store(position + 1, std::memory_order_release);
						return;
					}
				} else if (difference < 0) {
					// Full, so spill. Never drop.
					auto* node = new Spill{ std::move(a_value), spill.load(std::memory_order_relaxed) };
					while (!spill.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {
					}
					spilled.fetch_add(1, std::memory_order_relaxed);
					return;
				} else {
					position = head.load(std::memory_order_relaxed);
				}
			}
		}

		struct Cell
		{
			std::atomic<std::uint64_t> sequence{ 0 };
			T value{};
		};
		struct Spill
		{
			T value;
			Spill* next;
		};
		Cell* cells = new Cell[Capacity];  // never freed, see above
		std::atomic<std::uint64_t> head{ 0 };
		std::uint64_t tail = 0;  // the consumer's
		std::atomic<Spill*> spill{ nullptr };
		std::atomic<std::uint64_t> spilled{ 0 };
		void (*onPush)() = nullptr;  // set at construction, before the queue is shared
	};
}

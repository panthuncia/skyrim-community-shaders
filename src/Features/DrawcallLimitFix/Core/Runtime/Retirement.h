#pragma once

#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

#include <atomic>
#include <memory>
#include <utility>

namespace DCLF
{
	/**
	 * @brief Deferred reclamation by ownership: nothing is freed while a published version that may name it lives.
	 * The owner retires into the open node, and each publication holds the node opened with it. Each node holds the next newer one.
	 * When the last publication up to a node goes, on whatever thread, the batch returns and the owner Drains it.
	 * Batch must be movable, with Empty().
	 */
	template <class Batch>
	class RetirementChain
	{
		struct Returned
		{
			Batch batch;
			Returned* next = nullptr;
		};
		struct Node
		{
			explicit Node(RetirementChain* a_chain) :
				chain(a_chain) {}
			Node(const Node&) = delete;
			Node& operator=(const Node&) = delete;
			~Node()
			{
				if (!batch.Empty())
					chain->Return(std::move(batch));
				// Newer nodes go with this one when nothing else holds them. Done iteratively, not by recursion through the chain.
				auto newer = std::move(next);
				while (newer && newer.use_count() == 1) {
					auto after = std::move(newer->next);
					newer.reset();
					newer = std::move(after);
				}
			}
			RetirementChain* chain;
			Batch batch;
			std::shared_ptr<Node> next;
		};

	public:
		RetirementChain() :
			open(std::make_shared<Node>(this)) {}
		RetirementChain(const RetirementChain&) = delete;
		RetirementChain& operator=(const RetirementChain&) = delete;
		~RetirementChain()
		{
			open.reset();
			Drain([](Batch&&) {});
		}

		/** @brief Owner thread. Where things retired now go. */
		Batch& Open() { return open->batch; }
		/** @brief Owner thread, with a publication made now. The node it holds. */
		std::shared_ptr<const void> Publish()
		{
			auto node = std::make_shared<Node>(this);
			open->next = node;
			open = std::move(node);
			return open;
		}
		/** @brief Owner thread. Calls a_take(batch) for batches returned since, oldest first. Returns how many. */
		template <class F>
		std::size_t Drain(F&& a_take)
		{
			Returned* list = returned.exchange(nullptr, std::memory_order_acquire);
			Returned* ordered = nullptr;
			while (list) {
				auto* next = list->next;
				list->next = ordered;
				ordered = list;
				list = next;
			}
			std::size_t count = 0;
			while (ordered) {
				auto* next = ordered->next;
				a_take(std::move(ordered->batch));
				delete ordered;
				ordered = next;
				++count;
			}
			return count;
		}

	private:
		void Return(Batch&& a_batch)
		{
			auto* node = new Returned{ std::move(a_batch) };
			node->next = returned.load(std::memory_order_relaxed);
			while (!returned.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {}
		}

		std::shared_ptr<Node> open;
		std::atomic<Returned*> returned{ nullptr };
	};

	/**
	 * @brief Immutable publications one thread lets go of for another to drop, so the render thread never drops them.
	 * Any thread pushes, and one thread drops.
	 */
	template <class T>
	class Retired
	{
	public:
		void Push(std::shared_ptr<const T>&& a_retired)
		{
			if (a_retired)
				queue.Push(std::move(a_retired));
		}
		/** @brief The dropping thread. Lets go of everything pushed so far. */
		void Drop()
		{
			queue.Drain([](std::shared_ptr<const T>&& a_retired) { const auto dropped = std::move(a_retired); });
		}

	private:
		EventQueue<std::shared_ptr<const T>, 64> queue;
	};
}

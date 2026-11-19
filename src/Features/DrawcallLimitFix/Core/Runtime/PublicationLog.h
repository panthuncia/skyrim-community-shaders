#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace DCLF
{
	/**
	 * @brief A lock-free forward-linked log from one producer to one consumer that never loses a node.
	 * Unlike a latest-wins mailbox, a publication the frame's start never took still reaches the consumer.
	 * The producer never reads a node after linking it, and the consumer never frees the producer's last node, so they never touch the same node.
	 * The consumer may keep several cursors. Release frees the nodes before the one that lags.
	 */
	template <class T>
	class PublicationLog
	{
	public:
		struct Node
		{
			T value{};
			std::uint64_t sequence = 0;  // 1 for the first appended, 0 for the stub
			std::atomic<Node*> next{ nullptr };
		};

		PublicationLog() :
			stub(new Node{}), tail(stub) {}
		PublicationLog(const PublicationLog&) = delete;
		PublicationLog& operator=(const PublicationLog&) = delete;
		~PublicationLog()
		{
			for (Node* node = stub; node;) {
				Node* next = node->next.load(std::memory_order_acquire);
				delete node;
				node = next;
			}
		}

		/** @brief Producer. Appends a_value and returns its sequence. */
		std::uint64_t Append(T&& a_value)
		{
			auto* node = new Node{};
			node->value = std::move(a_value);
			node->sequence = ++appended;
			tail->next.store(node, std::memory_order_release);
			tail = node;
			return node->sequence;
		}
		/** @brief Producer. The last appended sequence, or 0. */
		std::uint64_t Appended() const { return appended; }

		/** @brief Consumer. The first node held, where a cursor starts. */
		Node* First() const { return stub; }
		/** @brief Consumer. The node after a_node, or null while nothing follows. */
		static Node* Next(Node* a_node) { return a_node->next.load(std::memory_order_acquire); }
		/** @brief Consumer. Frees every node before a_lagging, the cursor furthest behind, which becomes the first node held. */
		void Release(Node* a_lagging)
		{
			while (stub != a_lagging) {
				Node* next = stub->next.load(std::memory_order_acquire);
				if (!next)
					return;  // a_lagging is not in the log, a defect, so free nothing past the producer's last
				delete stub;
				stub = next;
			}
		}

	private:
		Node* stub;               // the consumer's
		Node* tail;               // the producer's
		std::uint64_t appended = 0;  // the producer's
	};
}

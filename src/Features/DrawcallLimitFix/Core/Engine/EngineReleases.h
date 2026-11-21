#pragma once

#include "RenderGraph/Async/EventQueue.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::EngineReleases
{
	/**
	 * @brief Engine references let go off the render thread, released on it at Present. Dropping the last reference to a scene graph
	 * object runs the engine's destructors, which belong on its main thread.
	 */
	inline RenderGraph::Frame::EventQueue<RE::NiPointer<RE::NiRefObject>> queue;
	inline RenderGraph::Frame::EventQueue<std::shared_ptr<void>> owners;

	// Scene work references go back through the retirement chain. A batch arrives here once no publication up to its node lives.
	// A reader that keeps only a publication's data, not its node, asks Alive. These are the newest publications pushed and released.
	inline std::atomic<std::uint64_t> pushedThrough{ 0 };
	inline std::atomic<std::uint64_t> releasedThrough{ 0 };

	/** @brief Any thread. */
	inline void Push(RE::NiPointer<RE::NiRefObject>&& a_reference)
	{
		if (a_reference)
			queue.Push(std::move(a_reference));
	}

	/** @brief Any thread. An owner whose last release may run the engine's destructors. */
	inline void PushOwner(std::shared_ptr<void>&& a_owner)
	{
		if (a_owner)
			owners.Push(std::move(a_owner));
	}

	/** @brief Before pushing a retired batch. a_stamp was the last publication that held its node. */
	inline void NoteReleased(std::uint64_t a_stamp)
	{
		std::uint64_t at = pushedThrough.load(std::memory_order_relaxed);
		while (at < a_stamp && !pushedThrough.compare_exchange_weak(at, a_stamp, std::memory_order_release, std::memory_order_relaxed)) {}
	}

	/**
	 * @brief Whether no object a_publication names was released. Read under an engine-read lease or on the render thread, where it
	 * stays true until the next Present.
	 */
	inline bool Alive(std::uint64_t a_publication) { return releasedThrough.load(std::memory_order_acquire) <= a_publication; }

	/** @brief Render thread, at Present. Releases every owner and reference pushed so far. */
	inline void Release()
	{
		// Take first, then release. Every stamp was noted before its push, so pushedThrough read after the take covers them all.
		static std::vector<std::shared_ptr<void>> takenOwners;
		static std::vector<RE::NiPointer<RE::NiRefObject>> takenReferences;
		owners.Drain([](std::shared_ptr<void>&& a_owner) { takenOwners.push_back(std::move(a_owner)); });
		queue.Drain([](RE::NiPointer<RE::NiRefObject>&& a_reference) { takenReferences.push_back(std::move(a_reference)); });
		releasedThrough.store(pushedThrough.load(std::memory_order_acquire), std::memory_order_release);
		takenOwners.clear();
		takenReferences.clear();
	}
}

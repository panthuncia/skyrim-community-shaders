#pragma once

#include "Snapshot/Retire.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::EngineReleases
{
	/**
	 * DCLF's engine references go through the snapshot retire queue's engine lane (Snapshot/Retire.h, roadmap phase 6). They are let
	 * go on the main thread after the update's frame jobs, once the GPU finished the newest render frame begun when they were pushed.
	 * Dropping the last reference to a scene graph object runs the engine's destructors, which belong on its main thread with no engine
	 * job running beside them.
	 */

	// Scene work references go back through the retirement chain. A batch arrives here once no publication up to its node lives.
	// A reader that keeps only a publication's data, not its node, asks Alive. These are the newest publications pushed and released.
	inline std::atomic<std::uint64_t> pushedThrough{ 0 };
	inline std::atomic<std::uint64_t> releasedThrough{ 0 };

	/** @brief Any thread. */
	inline void Push(RE::NiPointer<RE::NiRefObject>&& a_reference)
	{
		if (a_reference)
			Snapshot::Retire::AfterThisFrame(Snapshot::Retire::Lane::Engine, std::make_shared<const RE::NiPointer<RE::NiRefObject>>(std::move(a_reference)));
	}

	/** @brief Any thread. An owner whose last release may run the engine's destructors. */
	inline void PushOwner(std::shared_ptr<void>&& a_owner)
	{
		if (a_owner)
			Snapshot::Retire::AfterThisFrame(Snapshot::Retire::Lane::Engine, std::move(a_owner));
	}

	/**
	 * @brief Any thread. A retired batch's engine references and owners. a_stamp was the last publication that held its node: once
	 * they are let go, releasedThrough covers it.
	 */
	inline void PushRetired(std::uint64_t a_stamp, std::vector<RE::NiPointer<RE::NiRefObject>>&& a_references, std::vector<std::shared_ptr<void>>&& a_owners)
	{
		/** @brief Lets go of its references, then marks its publication released. Destroyed by the engine lane's drain. */
		struct Retired
		{
			std::uint64_t stamp = 0;
			std::vector<RE::NiPointer<RE::NiRefObject>> references;
			std::vector<std::shared_ptr<void>> owners;

			~Retired()
			{
				owners.clear();
				references.clear();
				std::uint64_t at = releasedThrough.load(std::memory_order_relaxed);
				while (at < stamp && !releasedThrough.compare_exchange_weak(at, stamp, std::memory_order_release, std::memory_order_relaxed)) {}
			}
		};
		std::uint64_t at = pushedThrough.load(std::memory_order_relaxed);
		while (at < a_stamp && !pushedThrough.compare_exchange_weak(at, a_stamp, std::memory_order_release, std::memory_order_relaxed)) {}
		auto retired = std::make_shared<Retired>();
		retired->references = std::move(a_references);
		retired->owners = std::move(a_owners);
		retired->stamp = a_stamp;
		Snapshot::Retire::AfterThisFrame(Snapshot::Retire::Lane::Engine, std::move(retired));
	}

	/**
	 * @brief Whether no object a_publication names was released. Read under an engine-read lease or on the main thread: releases happen
	 * only after the update's frame jobs, on the main thread, while the engine-read window is closed.
	 */
	inline bool Alive(std::uint64_t a_publication) { return releasedThrough.load(std::memory_order_acquire) <= a_publication; }
}

#pragma once

#include <cstdint>

/**
 * @brief Counts heap allocations per thread, for the in-process sampler.
 *
 * CommunityShaders.dll replaces the global operator new and delete, so every C++ allocation in CS, ORG and BasicRHI passes through.
 * Each counted thread writes only its own slot. Allocations by the system, the driver and other modules are not seen.
 */
namespace RenderGraph::Engine::Native::AllocCounter
{
	struct Totals
	{
		std::uint64_t allocs = 0;
		std::uint64_t bytes = 0;
		std::uint64_t frees = 0;
	};

	/** @brief Starts counting. Once on, counting stays on. Any thread. */
	void Enable();

	/** @brief Every kSiteEvery-th allocation of a tracked thread keeps its call stack. */
	constexpr std::uint32_t kSiteEvery = 16;
	constexpr std::uint32_t kSiteFrames = 20;
	constexpr std::uint32_t kSiteRings = 4;
	constexpr std::uint32_t kSiteRing = 32768;

	struct Site
	{
		std::uint32_t bytes = 0;
		std::uint32_t count = 0;
		void* frames[kSiteFrames]{};
	};

	/** @brief Tracks a thread's allocation sites in ring a_ring from now on. Call before Enable. */
	void TrackSites(std::uint32_t a_ring, std::uint32_t a_threadId);
	void StopSites();
	/** @brief How many sites ring a_ring kept. Past kSiteRing the oldest are overwritten. */
	std::uint32_t SiteCount(std::uint32_t a_ring);
	/** @brief How many sites ring a_ring was given in all, kept or overwritten. */
	std::uint32_t SiteWritten(std::uint32_t a_ring);
	const Site& SiteAt(std::uint32_t a_ring, std::uint32_t a_index);

	/** @brief A thread's totals so far, zero without a slot. Any thread. */
	Totals Of(std::uint32_t a_threadId);

	/** @brief Calls a_visit for every thread with a slot. Any thread. */
	template <class F>
	void ForEach(F&& a_visit);

	std::uint32_t SlotCount();
	void SlotAt(std::uint32_t a_index, std::uint32_t& a_threadId, Totals& a_totals);

	template <class F>
	void ForEach(F&& a_visit)
	{
		const std::uint32_t count = SlotCount();
		for (std::uint32_t i = 0; i < count; ++i) {
			std::uint32_t id = 0;
			Totals totals;
			SlotAt(i, id, totals);
			if (id)
				a_visit(id, totals);
		}
	}
}

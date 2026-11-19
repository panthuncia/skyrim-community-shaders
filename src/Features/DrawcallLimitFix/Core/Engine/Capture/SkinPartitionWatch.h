#pragma once

// Watches skin partitions about to be replaced outside the engine's writers.
//
// RaceMenu's body and face morphs deep-copy a geometry's NiSkinPartition, write the morphed vertices into the copy, and store it
// straight into NiSkinInstance::skinPartition, now or in an SKSE task later. No engine writer runs and skee sends no event.
// A detour on CreateDeepCopy names each partition being copied. For a few seconds after, FrameValues samples every item holding it,
// moving or not, so a morphed corpse or mannequin draws its new shape. Deep copies are rare, so this costs nothing in steady state.

#include <cstdint>
#include <vector>

namespace DCLF::SkinPartitionWatch
{
	/** @brief Installs the CreateDeepCopy detour at PostPostLoad. */
	void Install();

	/** @brief Any thread. Whether any recently copied partition is watched. One relaxed load. */
	bool Any();

	/** @brief The FrameValues producer. The watched partitions, never dereferenced. Expired ones are dropped first. */
	void Collect(std::vector<const void*>& a_out);

	/** @brief Partitions watched since start, and now. */
	struct Counts
	{
		std::uint64_t copies = 0;
		std::uint32_t watched = 0;
	};
	Counts GetCounts();
}

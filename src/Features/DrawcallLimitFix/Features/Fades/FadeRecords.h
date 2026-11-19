#pragma once

// Records only Fades uses. The root-state rows the cull also reads are in Core/Scene/Model/Records.h.

#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include <cstdint>

namespace DCLF
{
	/** @brief One animation update's inputs, as the fade writers record them. */
	struct AnimatedFadeInputs
	{
		float eye[3]{};
		float lodAdjust = 0.0f;
		std::int32_t counter = 0;
		float deltaTime = 0.0f;
		std::uint32_t valid = 0;
		std::uint32_t varied = 0;  // a later update in the batch had other inputs, but the first update's are used for all
	};

	/** @brief One root's update from FadeStateCS, which the parity check replays in C++. */
	struct FadeLogEntry
	{
		FadeNodeState before;
		FadeNodeState after;
		float centre[3]{};
		std::uint32_t root = ~0u;
	};
	static_assert(sizeof(FadeLogEntry) == 112);
	inline constexpr std::uint32_t kFadeLogEntries = 64;

	/**
	 * @brief A root whose fade crossed a milestone or whose leaf LOD changed. The host writes these onto the nodes in frame order.
	 * The list starts with a header of count, scene frame and two padding words. The scene frame is always written.
	 */
	struct FadeEvent
	{
		std::uint32_t root = 0;
		std::uint32_t generation = 0;  // a slot reused since has a new generation, so the event is stale
		std::uint32_t flags = 0;       // kFadeFlagMask bits
		float currentFade = 0.0f;
		std::uint32_t levels = 0;      // node +0x152 | +0x153 << 8
		float blend = 0.0f;            // node +0x14C, the LOD cross-fade
		std::uint32_t padding[2]{};
	};
	static_assert(sizeof(FadeEvent) == 32);
	inline constexpr std::uint32_t kFadeEventWords = sizeof(FadeEvent) / sizeof(std::uint32_t);
	inline constexpr std::uint32_t kFadeEventHeaderWords = 4;
	inline constexpr std::uint32_t kInitialFadeEvents = 1024;
}

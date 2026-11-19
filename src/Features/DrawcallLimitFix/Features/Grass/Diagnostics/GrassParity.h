#pragma once

// CS_DCLF_PERSISTENT_PARITY state for the grass mirror check.

#include <cstdint>
#include <string>

namespace DCLF::Features::Grass
{
	/** @brief Render thread. */
	struct GrassParityState
	{
		std::uint64_t checks = 0, shapes = 0, missing = 0, stale = 0, differ = 0, late = 0;
		std::uint64_t liveInstances = 0, mirroredInstances = 0;
		// Records placed as the cull places them, checked against its eye and distance. Nearest is -1 before any draw row.
		std::uint64_t withinDistance = 0;
		float nearest = -1.0f, maxDistance = 0.0f;
		std::string first;
		ankerl::unordered_dense::map<const void*, std::string> suspects;  // shapes that differed at the last check, and how
		std::uint32_t frame = ~0u;
	};

	GrassParityState& ParityState();
}

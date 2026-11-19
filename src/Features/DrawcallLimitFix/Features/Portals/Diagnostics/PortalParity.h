#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Diagnostics::PortalParity
{
	/**
	 * @brief The portal views' parity: at each Build, DCLF's programs against the engine's portal-graph entry as Main::Update's walk
	 * left it. Frames whose camera moved after the engine's walk are tallied apart. One line every 300 builds.
	 */
	void Install();

	struct Tally
	{
		std::uint64_t frames = 0, rooms = 0, roomsMissing = 0, roomsExtra = 0, programs = 0, programsDiffer = 0;
		std::uint64_t unboundDiffer = 0, flagDiffer = 0, accumDiffer = 0;
		// Unbound-space programs that match once rebuilt with the engine's plane states. Not a difference.
		std::uint64_t unboundEngineStates = 0;
		std::uint64_t ops = 0, sets = 0;  // what the compared programs held
		float largest = 0.0f;             // the largest plane difference among the programs that matched in structure
		std::uint64_t eyeMoved = 0, movedDiffer = 0;  // frames whose camera moved after the engine's walk, and their differences
		float largestMove = 0.0f;
		std::uint64_t turned = 0, boxChecks = 0, boxCornersDiffer = 0, boxSetsDiffer = 0;
		float largestCorner = 0.0f;
		std::string boxFirst;
		std::uint64_t planesDiffer = 0;
		std::string planesFirst;
		std::string occluderFirst;
		std::string first;
	};
	/** @brief Returns and resets the tally. */
	Tally Take();
}

#pragma once

// The views the engine culls whole because DCLF does not cover them yet. Each is a gap in DCLF's coverage, counted and warned about once.

#include <cstdint>
#include <string>

namespace DCLF::WholeCull
{
	/** @brief Why the engine culled a DCLF view whole this frame. */
	enum class Reason : std::uint8_t
	{
		SunNotCovered,      // a sun cascade not DCLF's, culled with the entry exclusion
		SunNoExclusion,     // no exclusion current either, so everything
		PointNotCovered,    // or no selection this frame
		PointPastMask,      // past the light mask's 32
		SpotNotCovered,
		SpotPastMask,
		PrimaryNoSnapshot,  // no candidates yet
		OcclusionNotTaken,  // the engine's SetupMask over every scene list
		Count
	};

	/** @brief Any thread. Counts a whole cull. The first of each reason is logged. */
	void Note(Reason a_reason);
	/** @brief The whole culls since the last call as a report line, or empty. */
	std::string Line();
}

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace DCLF::Features::Shadows::Local::LightCulls
{
	/**
	 * @brief The point lights' cull parity (CS_DCLF_PERSISTENT_PARITY), on frames whose culls skip nothing.
	 * Flags passes the skip would lose, casters outside the filter's kept children, and point light bits that differ from the engine's.
	 */
	void Install();

	/** @brief The counts since the last Take. The registration threads count. */
	struct Counts
	{
		std::uint64_t frames = 0, passes = 0, lost = 0, filterChecked = 0, filterMissed = 0;
		std::uint64_t compared = 0, agree = 0, engineOnly = 0, dclfOnly = 0, unhandled = 0;
		// The empty culls' bits (every light's predicted sphere) against the engine's, at each geometry's first clearing main registration.
		std::uint64_t sphereCompared = 0, sphereAgree = 0, sphereEngineOnly = 0, sphereDclfOnly = 0;
		std::string sphereFirstEngineOnly;
		std::array<std::uint64_t, 4> engineOnlyBy{}, dclfOnlyBy{};  // by the differing light's reach (untested, sphere, portals, not reached)
		std::array<std::uint64_t, 16> lostByReject{};
		std::string firstLost, filterFirstMissed;
	};
	Counts Take();
}

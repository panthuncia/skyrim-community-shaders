#pragma once

#include <cstdint>

namespace DCLF::Diagnostics
{
	/**
	 * @brief Which subjects this run observes, from the CS_DCLF_* switches.
	 * Read once before the first frame, so nothing changes under a running frame. Observers that change what the main path does
	 * are Policy.h questions instead.
	 */
	struct Observation
	{
		// Parity checks compare a live path against a reference and flag each mismatch with "<-".
		bool parity = false;          // CS_DCLF_PARITY: set parity, the kept stores against a rebuild, and the mirror's probe every frame
		bool capture = false;         // CS_DCLF_CAPTURE_PARITY, the tables against the engine's own lighting draws
		bool engineDrawsAll = false;  // CS_DCLF_PARITY_BOTH, nothing is withheld from the engine

		// Statistics.
		bool stats = false;         // CS_DCLF_STATS, the periodic report
		bool passStats = false;     // CS_DCLF_PASS_STATS, per-pass counts
		bool profile = false;       // CS_DCLF_PROFILE, scene-phase timings
		bool releaseGuard = false;  // CS_DCLF_RELEASE_GUARD, engine releases on DCLF's threads
	};

	/** @brief The run's observation, read from the switches on first use. */
	const Observation& Observe();

	/** @brief Parity checks run every 60 frames or passes, each at its own offset so their rebuilds do not share a frame. */
	constexpr bool ParityDue(std::uint64_t a_serial, std::uint32_t a_offset = 0) { return a_serial % 60 == a_offset; }
}

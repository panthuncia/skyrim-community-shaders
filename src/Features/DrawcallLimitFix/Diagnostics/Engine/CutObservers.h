#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace DCLF::Diagnostics::CutObservers
{
	/**
	 * @brief Primary cull observers under CS_DCLF_PERSISTENT_PARITY.
	 * "<- CUT" compares the kept cut with one planned whole every 60th frame. "<- STAND-IN WALK" flags geometry handed over from an
	 * entry the walk leaves alone. "<- LIGHT MASKS" flags owned members whose activeLightMask is not 0 after registration.
	 * "<- LIST FILTER" checks the filter's dry run and the engine's whole lists on frames they are built whole.
	 */
	void Install(bool a_scene);

	/** @brief The observers' counts since the last Take. Render thread. */
	struct Counts
	{
		std::uint64_t cutChecks = 0, cutMismatches = 0;
		std::uint64_t cutLagging = 0;  // differed at one check only, the snapshot behind the engine
		std::uint64_t walkMissed = 0;
		std::string walkMissedFirst;
		std::uint64_t maskChecked = 0, maskSun = 0, maskOther = 0;
		std::string maskFirst;
		std::uint64_t filterChecked = 0, filterMissed = 0;
		std::uint64_t listChecks = 0, listMissing = 0, listExtra = 0;  // missing is the engine's not in DCLF's, extra is DCLF's not hidden and not the engine's
		std::string listFirst;
		// The kept lists against a rebuild's. Missing is a root a rebuild lists, extra one it does not.
		std::uint64_t keptChecks = 0, keptMissing = 0, keptExtra = 0;
		std::string keptFirst;
	};
	Counts Take();
}

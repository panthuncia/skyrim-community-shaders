#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Diagnostics::LightSelectionParity
{
	/** @brief Tally of DCLF's light selection against the engine's (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY). */
	void Install();

	struct Tally
	{
		std::uint64_t frames = 0, kept = 0, slotsDiffer = 0, masksDiffer = 0, countsDiffer = 0, focusDiffer = 0, focusHosts = 0, framesDiffer = 0;
		std::uint64_t roomTests = 0, roomEntriesApart = 0, roomsDiffer = 0, roomsMovedDiffer = 0, roomSetsDiffer = 0, roomSetsMovedDiffer = 0;
		std::uint64_t roomWalks[3]{};          // By PortalViews::LightWalk.
		std::uint64_t roomProcessesOther = 0;  // Walked lights whose room process is not what WalkLight assumes.
		std::string first, roomSetsFirst, roomProcessFirst;
	};
	/** @brief Returns the tally since the last call and resets it. */
	Tally Take();
}

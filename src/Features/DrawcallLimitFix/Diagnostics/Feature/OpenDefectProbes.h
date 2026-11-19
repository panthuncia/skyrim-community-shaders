#pragma once

namespace DCLF
{
	// Probes for open defects. DrawcallLimitFix::ProbeOpaqueTarget is defined with them.
	/**
	 * @brief Checks the player's skeletons against the frame's claims. The hidden third-person skeleton and the first-person one
	 * must have no geometry claimed for the main view. A claim lasting past 8 frames of latency is drawn where the engine draws nothing.
	 */
	void ObservePlayerClaims();
}

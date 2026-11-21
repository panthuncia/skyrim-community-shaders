#pragma once

#include <cstdint>

namespace DCLF::Policy
{
	/**
	 * @brief Questions where an observer changes what the main path does, such as a parity that needs the engine's own result.
	 * With every CS_DCLF_* parity switch off, each answer is the normal path's. Policy.cpp derives them from Observation.
	 */

	/** @brief Withhold the set's members from the engine's views, else both draw them. True on the normal path. */
	bool WithholdFromEngine();

	/** @brief Check a slice of the tracked set against the live scene graph and drop entries whose detach was missed. False on the normal path. */
	bool ValidateTrackedSlice();

	/** @brief Check bound records' slots after a pass that freed one or on a_pass, and neutralise a record naming a slot no member holds. False on the normal path. */
	bool NeutraliseDanglingSlots(bool a_slotsFreed, std::uint64_t a_pass);
}

#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace DCLF::Diagnostics::Registrations
{
	/**
	 * @brief Set parity observers of the member passes the main camera's registration withholds. Any registration thread.
	 * "<- FORWARD" counts blended hint 1 passes and "<- REFRACTION" hint 4 passes, compared with what the colour segment found in view.
	 */
	void Install();

	/** @brief Blended passes withheld since the last call. Render thread, once a colour epoch. 0 without the parity. */
	std::uint32_t TakeForward();
	/** @brief Refraction passes withheld since the last call. Render thread, once a colour epoch. 0 without the parity. */
	std::uint32_t TakeRefraction();
}

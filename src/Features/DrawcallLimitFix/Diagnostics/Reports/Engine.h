#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Reports
{

	/** @brief The primary cull's report lines. Render thread, Present. */
	void PrimaryCullLines(std::uint32_t a_frame);

	/** @brief Entries with engine-drawn parts the engine's culls took per frame, over the window PrimaryCullLines just took. Render thread. */
	double CutMixedPerFrame();

	/** @brief The engine-read window's closes, waits and refused leases. Empty while nothing happened. */
	std::string EngineReadWindowLine();
	/** @brief Engine releases on DCLF's threads. Empty while not installed. */
	std::string ReleaseGuardLine();
	/** @brief The animation gate against DCLF's prediction, newline-terminated. Empty without the switch. */
	std::string AnimRateLine();
}

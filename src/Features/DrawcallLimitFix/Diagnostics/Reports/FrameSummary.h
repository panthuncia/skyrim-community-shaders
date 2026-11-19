#pragma once

// The menu's "This frame" lines, built in one place so the diagnostic capture writes the same text the panel shows.

#include <string>
#include <vector>

namespace DCLF::Reports
{
	/** @brief The "This frame" panel's lines. Reads only, no counter is reset. Render thread. */
	std::vector<std::string> FrameSummary();
}

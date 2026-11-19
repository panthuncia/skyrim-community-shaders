#pragma once

// Container sizes for the diagnostic capture, for Fades' state outside the scene lane.

#include <string>
#include <vector>

namespace DCLF::Fades
{
	/** @brief Render thread. One "<name> <value>" line per growable container. Takes the write-back's census lock once. */
	std::vector<std::string> RenderGrowthLines();
}

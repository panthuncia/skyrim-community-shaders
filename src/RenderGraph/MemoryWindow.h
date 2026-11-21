#pragma once

#include <cstddef>
#include <string>

/**
 * @brief The VRAM attribution window and the render graph's memory report line.
 * The report line reads only ORG's tracked buffers and textures.
 */
namespace RenderGraphMemory
{
	/** @brief UI thread. */
	void OpenWindow();
	/** @brief Draws the window while it is open. UI thread, inside an ImGui frame. */
	void DrawWindow();

	/**
	 * @brief One line of tracked device-local memory, with the largest name families and names. Empty without a graph. Any thread.
	 * A family drops digits and ends at the first '-' after the last '.', so "cs.dclf.grass-instances" is "cs.dclf.grass".
	 */
	std::string ReportLine(std::size_t a_families, std::size_t a_names);
}


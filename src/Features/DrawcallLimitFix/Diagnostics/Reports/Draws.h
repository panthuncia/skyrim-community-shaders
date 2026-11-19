#pragma once

#include <array>
#include <cstdint>
#include <string>

// The draws area's report lines, taken from the modules' counters at the render thread's report.

namespace DCLF::Reports
{
	/** @brief The frame values' producer and its parity, or empty when nothing ran. */
	std::string FrameValuesLine();
	/** @brief Frame data through the render thread, bytes per frame by where, largest first. Empty when none. */
	std::string FrameDataLine();
	/** @brief Logs the constant tables against ShaderCache's since the last call. */
	void ConstantTableLines();
	/** @brief Main pass pipeline statistics per frame, over a_samples frames each. */
	void PassStatsLine(const std::array<std::array<std::uint64_t, 4>, 4>& a_totals, const std::array<std::uint32_t, 4>& a_samples);
	/** @brief Logs the colour commits whose sun test ran without the sun's cascades. */
	void ColourSunTestLine(std::uint32_t a_empty);
	/**
	 * @brief The CPU pipeline's window, or empty when no CPU frame ran. Per view cull time, batches and draws, and per site its record
	 * time and waits. A site whose callbacks drew nothing all window is tagged.
	 */
	std::string CpuPipelineLine();
}

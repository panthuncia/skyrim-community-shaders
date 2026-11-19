#pragma once

// The culling counters' graph pass, which copies the colour epoch's count words to the host every execution.

namespace DCLF::Diagnostics
{
	struct MainGraphPasses;
	/** @brief Adds the colour epoch's counter readback after colour. Always on. */
	void AppendCullCounterPass(const MainGraphPasses& a_passes);
}

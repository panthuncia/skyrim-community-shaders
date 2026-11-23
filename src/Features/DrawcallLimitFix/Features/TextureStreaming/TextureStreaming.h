#pragma once

// Texture streaming's part in DCLF. The core in src/TextureStreaming knows nothing of DCLF. This adds window builds as graph work,
// the swaps into material bindings, and the demand from scene bounds plus shader feedback. All but the feedback pass run on the
// streaming worker's low-priority thread.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <string>

namespace DCLF::Features::Streaming
{
	Api::Feature Feature();

	/** @brief A test's: logs how many material views draw their shell's current window, and samples of those that do not. */
	void ReportBindings();

	/** @brief CS_DCLF_STATS: the window builds since the last call, base windows and the others apart, as a log line. Empty with none. */
	std::string TakeWindowTimes();
}

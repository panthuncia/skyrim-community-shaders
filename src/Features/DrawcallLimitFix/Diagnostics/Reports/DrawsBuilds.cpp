#include "Draws.h"
#include "Schedule.h"

#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"

// The draws' build lines for the render thread's report.

namespace DCLF::Reports
{
	void PassStatsLine(const std::array<std::array<std::uint64_t, 4>, 4>& a_totals, const std::array<std::uint32_t, 4>& a_samples)
	{
		static constexpr const char* kNames[4] = { "Z-prepass", "Z-prepass phase 2", "colour", "decals" };
		std::string text;
		for (std::uint32_t k = 0; k < 4; ++k) {
			if (!a_samples[k])
				continue;
			const double n = a_samples[k];
			text += fmt::format("; {}: {:.0f} vertices, {:.0f} primitives, {:.0f} VS invocations, {:.0f} PS invocations", kNames[k], a_totals[k][0] / n,
				a_totals[k][1] / n, a_totals[k][2] / n, a_totals[k][3] / n);
		}
		logger::info("[DCLF] pass statistics, per frame{}", text);
	}

	void ColourSunTestLine(std::uint32_t a_empty)
	{
		logger::info("[DCLF] colour sun test: {} of {} frames without the sun's cascades{}", a_empty, Interval(), a_empty ? " <- NO CASCADES" : " <- OK");
	}
}

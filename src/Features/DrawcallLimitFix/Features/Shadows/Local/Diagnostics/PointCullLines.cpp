// The point lights' culls' report lines: Accumulate timing and, with the skip installed, the culls', category filter's and bits'
// lines.

#include "Features/DrawcallLimitFix/Features/Shadows/Local/Local.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LocalLightCull.h"

#include <ranges>
#include <string>

#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

namespace DCLF::Features::Shadows::Local
{
	namespace
	{
		/** @brief The point lights' Accumulate timing and, with the skip installed, its culls', category filter's and bits' lines. */
		std::string LocalLightLines()
		{
			auto c = LocalLightCull::Take();
			const auto timing = fmt::format("[DCLF] point lights' Accumulate: {} calls, {:.3f} ms in all", c.accumulateCalls, c.accumulateTicks * Reports::MsPerTick());
			if (!c.installed)
				return timing;
			const auto text = fmt::format("[DCLF] point lights' shadow culls: {} frames, {} with the exclusion ({} none built, {} the mode's claims not live, {} without candidates, {} translated to newer candidates); {} entries visited, {} skipped",
				c.frames, c.live, c.noExclusion, c.noClaims, c.stale, c.masked, c.visited, c.skipped);
			std::string list;
			if (c.filterInstalled) {
				const double filtered = static_cast<double>(c.filtered);
				list = fmt::format("\n[DCLF] point lights' category filter: {} lights, {} filtered, {} walked whole (frames without it); {:.1f} filtered nodes a light ({} now), {:.0f} children culled and {:.0f} cut a light, {} dirty nodes walked; {} builds on the pool ({} nodes, {:.3f} ms each; {} for an older verdict, {} refused a lease)",
					c.lights, c.filtered, c.walkedNotLive, c.filtered ? static_cast<double>(c.nodes) / filtered : 0.0, c.filterNodes,
					c.filtered ? static_cast<double>(c.children) / filtered : 0.0, c.filtered ? static_cast<double>(c.cut) / filtered : 0.0, c.dirtyNodes, c.builds,
					c.nodesRebuilt, c.builds ? static_cast<double>(c.buildTicks) * Reports::MsPerTick() / static_cast<double>(c.builds) : 0.0, c.buildsStale, c.buildsRefused);
			}
			list += fmt::format("\n[DCLF] point lights' bits for engine main passes under cut entries: {} lights ({} past the mask's 32), {} geometries given them ({} any)",
				c.bitLights, c.overflow, c.given, c.withBits);
			list += fmt::format("\n[DCLF] point lights' empty culls (no engine cull: DCLF draws every paraboloid view): {} of {} lights ({} with a view not DCLF's, {} before DCLF made their views (no shadow yet), {} portal-strict kept native, {} past the mask's 32; {} portal-strict, their bits' gates {} tested, {} refused, {} without a portal graph, {} without a context{}); "
								"{} main registrations given their bits ({} any), {} without a slot{}",
				c.emptyLights, c.bitLights + c.overflow, c.emptyNotCovered, c.emptyLate, c.emptyPortalStrict, c.overflow, c.strictLights, c.strictTested, c.strictRefused, c.strictNoGraph, c.strictNoContext,
				c.strictNoContext ? " <- STRICT CONTEXT" : "", c.emptyGiven, c.emptyWithBits, c.emptyUnstamped, c.emptyUnstamped ? " <- STAMPS" : "");
			list += fmt::format("\n[DCLF] spot lights' shadow culls: {} Accumulate calls, {:.3f} ms in all; {} culled empty (DCLF draws the view; bits from the process's planes), {} with a view not DCLF's, {} portal-strict kept native",
				c.spotCalls, c.spotTicks * Reports::MsPerTick(), c.emptySpots, c.spotsNotCovered, c.spotsStrict);
			return timing + "\n" + text + list;
		}
	}

	void PointCullLines(std::uint32_t)
	{
		// Only on the primary cull's report frames (the point's): the counters stand until then.
		for (const auto& line : std::views::split(LocalLightLines(), '\n'))
			if (const std::string text(line.begin(), line.end()); !text.empty())
				logger::info("{}", text);
	}
}

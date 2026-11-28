#include "Draws.h"

#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameData.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/LatePoses/LatePoseOverrides.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"

// Frame values and frame data report lines.

namespace DCLF::Reports
{
	std::string FrameValuesLine()
	{
		const auto c = FrameValues::Get().TakeCounters();
		if (!c.frames)
			return {};
		const double n = static_cast<double>(c.frames);
		auto line = fmt::format("[DCLF] frame values: {} frames; a frame {:.0f} items sampled ({:.1f} written slots kept for their transforms to settle), {:.1f} fade roots seeded, {:.1f} rows changed, {:.0f} skins ({:.1f} palette rows changed), "
								"{:.1f} shading items sampled ({:.1f} rows changed, {:.1f} wetness changed), "
								"{:.1f} runs ({:.1f} KB) sent; {:.0f} us sampling, {:.0f} us sending, at most {} us; "
								"{} refused leases, {} palette size defects, {} failures; {} producers wrote a copy a frame in flight still bound ({} copies a class), "
								"waiting for it on the GPU",
			c.frames, c.sampled / n, c.settled / n, c.seeded / n, c.rowsChanged / n, c.skins / n, c.paletteRowsChanged / n, c.shadingSampled / n, c.shadingChanged / n,
			c.wetnessChanged / n, c.runs / n, c.bytes / n / 1024.0, c.sampleUs / n, c.uploadUs / n, c.maxTotalUs, c.refused, c.defects,
			c.failures, c.reusedInFlight, PooledBackings::SceneCopies());
		if (c.stillRows || c.stillBlocks || c.aheadUploads)
			line += fmt::format("; {} rows and {} palette blocks drawn still (repeats), {} uploads sent a later ticket's rows", c.stillRows, c.stillBlocks, c.aheadUploads);
		// Late poses. Hidden nodes found, and the plans and rows they shrank.
		if (const auto late = LatePoses::TakeReport(); !late.empty())
			line += "; " + late;
		// The placement deltas the producers applied onto their kept set.
		if (c.plans)
			line += fmt::format("; {} placement plans applied ({} resets, {:.1f} items a plan)", c.plans, c.planResets, static_cast<double>(c.planItems) / static_cast<double>(c.plans));
		if (auto& s = Diagnostics::DrawsState::Get().frameValues; s.shadingChecks) {
			line += fmt::format("; {} resident shading rows checked, {} never sampled{}{}", s.shadingChecks, s.shadingUnsampled,
				s.shadingUnsampled ? " <- UNSAMPLED; first: " : " <- OK", s.shadingUnsampled ? s.shadingFirst : std::string());
			s.shadingChecks = s.shadingUnsampled = 0;
			s.shadingFirst.clear();
		}
		// Row set pools, built-in then registered.
		line += "\n[DCLF] frame values' pools (D6):";
		const auto pools = FrameValues::Get().TakePoolStats();
		for (std::size_t p = 0; p < pools.size(); ++p) {
			const auto& pool = pools[p];
			line += fmt::format("{} {} {:.2f} acquires, {:.1f} runs ({:.1f} KB) caught up a frame, {} made; {} live ({} leased, {} free, {:.1f} MB), floor {}", p ? ";" : "",
				pool.name, pool.acquires / n, pool.runs / n, pool.bytes / n / 1024.0, pool.made, pool.live, pool.leased, pool.free, pool.liveBytes / (1024.0 * 1024.0),
				pool.floor);
		}
		return line;
	}

	std::string FrameDataLine()
	{
		auto taken = FrameData::Take();
		if (!taken.frames)
			return {};
		const double n = static_cast<double>(taken.frames);
		std::vector<std::pair<const std::string*, FrameData::Counts>> sorted;
		std::uint64_t total = 0, writes = 0;
		for (const auto& [where, c] : taken.counts) {
			sorted.emplace_back(&where, c);
			total += c.bytes;
			writes += c.writes;
		}
		std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
		auto line = fmt::format("[DCLF] frame data through the render thread: {:.1f} KB a frame in {:.0f} writes over {} frames; by where:", total / n / 1024.0,
			writes / n, taken.frames);
		for (const auto& [where, c] : sorted)
			line += fmt::format(" {} {:.2f} KB ({:.1f} writes);", *where, c.bytes / n / 1024.0, c.writes / n);
		line.pop_back();
		return line;
	}
}

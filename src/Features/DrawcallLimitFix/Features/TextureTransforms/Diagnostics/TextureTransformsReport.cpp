// The texture transforms pool's stats line (CS_DCLF_STATS), called by the frame's producer.

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransformsPool.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::TextureTransforms
{
	namespace
	{
		// The producers run one at a time in frame order.
		std::uint64_t lineFrames = 0, lineEntries = 0, lineChanged = 0, lineRefused = 0, lineVariables = 0, lineVariablesChanged = 0;
	}

	// What the rows carrying TexcoordOffset or a declared frame variable cost a frame and how many moved.
	void CountPoolLine(std::size_t a_count, std::uint64_t a_changed, std::uint64_t a_refused, std::size_t a_variables, std::uint64_t a_variablesChanged)
	{
		++lineFrames;
		lineEntries += a_count;
		lineChanged += a_changed;
		lineRefused += a_refused;
		lineVariables += a_variables;
		lineVariablesChanged += a_variablesChanged;
		if (lineFrames < 300)
			return;
		if (Diagnostics::Observe().stats) {
			const double n = static_cast<double>(lineFrames);
			// The list's stages since the last line: built by the builder (rows left out by reason), carried by a payload, installed.
			namespace stats = TextureTransformStats;
			const auto take = [](std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
			const auto built = take(stats::built), builtEntries = take(stats::builtEntries), builtVariables = take(stats::builtVariables), unpacked = take(stats::rowsUnpacked),
					   noTexcoord = take(stats::rowsNoTexcoord), freeSlot = take(stats::rowsFreeSlot), stale = take(stats::rowsStale);
			const auto published = take(stats::published), publishedEntries = take(stats::publishedEntries), publishedNone = take(stats::publishedNone);
			const auto adopted = take(stats::adopted), adoptedEntries = take(stats::adoptedEntries), adoptedNone = take(stats::adoptedNone);
			const auto per = [](std::uint64_t a_sum, std::uint64_t a_count) { return a_count ? static_cast<double>(a_sum) / static_cast<double>(a_count) : 0.0; };
			logger::info("[DCLF] frame values' texture transforms: {} frames; a frame {:.0f} material rows sampled, {:.1f} changed (scrolled) and sent, {} unread "
						 "(refused leases, a producer a frame behind); of them frame variables {:.0f} sampled, {:.1f} changed; lists built {} ({:.0f} entries, "
						 "{:.0f} frame variables; a build left out {:.0f} unpacked, {:.0f} without TexcoordOffset or a frame variable, {:.0f} free slots, {:.0f} stale), "
						 "carried {} ({:.0f} entries, {} main payloads without one{}), installed {} ({:.0f} entries, {} none)",
				lineFrames, lineEntries / n, lineChanged / n, lineRefused, lineVariables / n, lineVariablesChanged / n, built, per(builtEntries, built),
				per(builtVariables, built), per(unpacked, built), per(noTexcoord, built), per(freeSlot, built), per(stale, built), published,
				per(publishedEntries, published), publishedNone, publishedNone ? " <- LIST LOST" : "", adopted, per(adoptedEntries, adopted), adoptedNone);
		}
		lineFrames = lineEntries = lineChanged = lineRefused = lineVariables = lineVariablesChanged = 0;
	}
}

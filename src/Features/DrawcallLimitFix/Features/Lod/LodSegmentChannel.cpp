// The segment events' channel and component. The component copies each named shape's drawn ranges from the mirror
// into Core's kept ranges, which the walk reads as one geometry slot per range.

#include "Features/DrawcallLimitFix/Features/Lod/LodSegmentEvents.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodSegments.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"

#include <memory>
#include <span>
#include <vector>

namespace DCLF::Features::Lod
{
	namespace
	{
		Api::EngineChannelId segmentChannel;

		std::shared_ptr<void> MakeState()
		{
			return std::make_shared<SegmentState>();
		}

		void ApplySegmentEvents(const Api::CoordinatorComponentContext& a_context, Api::EngineChannelId a_channel, std::span<const void* const> a_keys)
		{
			if (a_channel != segmentChannel)
				return;
			auto& stats = a_context.State<SegmentState>().stats;
			for (const void* key : a_keys) {
				++stats.events;
				// Only a tracked shape is known to be alive.
				const auto* kept = a_context.EntryRangesOf(key);
				if (!kept)
					continue;
				const auto* mirrored = a_context.MirroredRanges(key);
				std::vector<Api::EntryRange> next = mirrored ? *mirrored : std::vector<Api::EntryRange>{};
				const bool changed = next != *kept;
				if (changed)
					++stats.changed;
				a_context.SetEntryRanges(key, std::move(next), changed);
			}
		}

		void SegmentLines(const Api::CoordinatorComponentContext& a_context, std::string& a_text)
		{
			auto& segments = a_context.State<SegmentState>();
			auto& l = segments.stats;
			const auto& ranges = a_context.EntryRanges();
			if (!l.events && ranges.empty())
				return;
			std::size_t partial = 0;
			for (const auto& [shape, shapeRanges] : ranges) {
				const std::uint32_t whole = LodSegments::At<std::uint16_t>(shape, LodSegments::kTriangleCount) * 3u;
				partial += shapeRanges.size() != 1 || shapeRanges[0].firstIndex != 0 || shapeRanges[0].indexCount != whole ? 1 : 0;
			}
			a_text += fmt::format("[DCLF] object LOD segments: {} shapes ({} partly hidden){}; {} segment events, {} drawn-range changes\n",
				ranges.size(), partial, SegmentHooksInstalled() ? "" : ", events not installed", l.events, l.changed);
			l = {};
		}
	}

	void RegisterSegmentChannel(Api::InstallContext& a_context)
	{
		// The ranges themselves arrive through the mirror's update. This channel only names the shapes.
		segmentChannel = a_context.RegisterEngineChannel({ .name = "object LOD segments",
			.kind = Api::ChannelKind::Keys,
			.apply = Api::ChannelApply::AfterSwitches,
			.namesKeys = true,
			.drain = &DrainSegmentEvents });
		const auto component = a_context.RegisterCoordinatorComponent({ .name = "object LOD segments",
			.make = &MakeState,
			.events = { .keys = &ApplySegmentEvents } });
		a_context.RegisterReportLines({ .name = "object LOD segments",
			.point = Api::ReportPoint::SceneCoordinatorSegments,
			.component = component,
			.componentText = &SegmentLines });
	}
}

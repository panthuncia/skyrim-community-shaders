#include "RenderGraph/Frame/Starvation.h"

#include "RenderGraph/Frame/FrameDriver.h"

#include <string>

namespace RenderGraph::Frame
{
	StarvationWatch::StarvationWatch(std::string_view a_what) :
		org::async::StarvationWatch(std::string(a_what), kStarvedAfter, [](std::string_view a_watched, std::string_view a_why, std::uint64_t a_misses) {
			Driver::NoteTapError(Driver::TapError::Starved, SegmentId::Count,
				fmt::format("{} has not landed: {} misses with no new input since (last: {})", a_watched, a_misses, a_why));
		})
	{}
}

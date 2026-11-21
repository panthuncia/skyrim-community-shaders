#include "RenderGraph/Frame/Starvation.h"

#include "RenderGraph/Frame/FrameDriver.h"

namespace RenderGraph::Frame
{
	bool StarvationWatch::NoteMiss(std::string_view a_why, SegmentId a_segment)
	{
		if (++misses <= kStarvedAfter)
			return false;
		Driver::NoteTapError(Driver::TapError::Starved, a_segment,
			fmt::format("{} has not landed: {} misses with no new input since (last: {})", what, misses, a_why));
		misses = 0;
		return true;
	}
}

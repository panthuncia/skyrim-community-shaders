#pragma once

#include "RenderGraph/Frame/SegmentId.h"
#include "RenderGraph/FrameClock.h"

#include <ORGModuleServices/Async/StarvationWatch.h>

#include <cstdint>
#include <string_view>

namespace RenderGraph::Frame
{
	/**
	* @brief The one starvation rule (cleanup plan B10), on org::async::StarvationWatch. Work that is asked for again and again with
	* no new input never lands: that is a schedule bug, not a wait. The owner notes each new input and each miss (NoteInput,
	* NoteMiss). Past kStarvedAfter misses since the last input, a miss is a TapError::Starved, and the count starts again.
	*
	* One thread owns a watch (the render owner for the frame's). It holds no lock.
	*/
	class StarvationWatch : public org::async::StarvationWatch
	{
	public:
		/** @brief Misses since the last input before one is a tap error. */
		static constexpr std::uint64_t kStarvedAfter = 4 * FrameClock::kSlots;

		/** @brief a_what names the watched work in the tap error, as "the frame revision's member recordings". */
		explicit StarvationWatch(std::string_view a_what);
	};
}

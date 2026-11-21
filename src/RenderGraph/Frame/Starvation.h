#pragma once

#include "RenderGraph/Frame/SegmentId.h"
#include "RenderGraph/FrameClock.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace RenderGraph::Frame
{
	/**
	* @brief The one starvation rule (cleanup plan B10). Work that is asked for again and again with no new input never lands:
	* that is a schedule bug, not a wait. The owner notes each new input and each miss. Past kStarvedAfter misses since the last
	* input, a miss is a TapError::Starved, and the count starts again.
	*
	* One thread owns a watch (the render owner for the frame's). It holds no lock.
	*/
	class StarvationWatch
	{
	public:
		/** @brief Misses since the last input before one is a tap error. */
		static constexpr std::uint64_t kStarvedAfter = 4 * FrameClock::kSlots;

		/** @brief a_what names the watched work in the tap error, as "the frame revision's member recordings". */
		explicit StarvationWatch(std::string_view a_what) :
			what(a_what) {}

		/** @brief A new input: misses before it are not starvation. */
		void NoteInput() { misses = 0; }

		/**
		* @brief One miss: the work was dropped, or came for other inputs, and is asked for again. a_why says what happened.
		* Returns true when this miss was the tap error.
		*/
		bool NoteMiss(std::string_view a_why, SegmentId a_segment = SegmentId::Count);

		/** @brief Misses since the last input. */
		std::uint64_t Misses() const { return misses; }

	private:
		std::string what;
		std::uint64_t misses = 0;
	};
}

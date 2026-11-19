#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include <Render/RunRef.h>

namespace org
{
	class LatchBlock;
}

namespace DCLF
{
	/**
	 * @brief Counts the bytes the render thread writes for the GPU each frame, by destination. Fed from latched copies, staged
	 * uploads and latch writes. Work sent from workers is not counted.
	 */
	namespace FrameData
	{
		/** @brief Whether writes are counted, only under CS_DCLF_STATS. Check before building a name for Note, since names cost a string. */
		bool Enabled();
		/** @brief Render thread, at Present. Ends the frame. */
		void EndFrame();
		/** @brief Bytes the render thread wrote to a_where this frame. Ignored on any other thread. */
		void Note(std::string_view a_where, std::size_t a_bytes);
		struct Counts
		{
			std::uint64_t bytes = 0, writes = 0;
		};
		/** @brief The counts since the last take, over a number of frames. */
		struct Taken
		{
			std::uint64_t frames = 0;
			std::map<std::string, Counts, std::less<>> counts;
		};
		/** @brief Takes and resets the counts. Returns nothing before a frame wrote. */
		Taken Take();
	}

	/** @brief org::LatchBlock::Write, counted under "latch: a_what". */
	void LatchWrite(const org::LatchBlock& a_block, std::string_view a_what, const org::RunRef& a_run, std::uint32_t a_offset, std::span<const std::byte> a_bytes);
	template <class T>
	void LatchWriteValue(const org::LatchBlock& a_block, std::string_view a_what, const org::RunRef& a_run, std::uint32_t a_offset, const T& a_value)
	{
		LatchWrite(a_block, a_what, a_run, a_offset, std::as_bytes(std::span<const T, 1>(&a_value, 1)));
	}
}

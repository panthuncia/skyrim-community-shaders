#pragma once

// The render thread's frame number, which diagnostics and features stamp what they see with. Any thread reads it.

#include <atomic>
#include <cstdint>

namespace DCLF::FrameClock
{
	namespace Detail
	{
		inline std::atomic<std::uint32_t> frame{ 0 };
	}

	/** @brief Render thread, at the frame's start. */
	inline void Set(std::uint32_t a_frame) { Detail::frame.store(a_frame, std::memory_order_relaxed); }
	/** @brief Any thread. The render thread's frame as last set, 0 before the first. */
	inline std::uint32_t Frame() { return Detail::frame.load(std::memory_order_relaxed); }
}

#pragma once

// DCLF's scene step, which diagnostics and features stamp what they see with. Any thread reads it.
// It moves at each scene BeginFrame and holds in menus and loads. It is not the CS frame (RenderGraph::FrameClock).

#include <atomic>
#include <cstdint>

namespace DCLF::SceneClock
{
	namespace Detail
	{
		inline std::atomic<std::uint32_t> step{ 0 };
	}

	/** @brief Render thread, at the frame's start. */
	inline void Set(std::uint32_t a_step) { Detail::step.store(a_step, std::memory_order_relaxed); }
	/** @brief Any thread. The scene step as last set, 0 before the first. */
	inline std::uint32_t Step() { return Detail::step.load(std::memory_order_relaxed); }
}

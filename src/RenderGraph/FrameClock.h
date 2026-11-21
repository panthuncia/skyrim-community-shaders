#pragma once

#include <ORGModuleServices/Async/FrameClock.h>

#include <cstdint>

class RenderGraphRuntime;

namespace RenderGraph
{
	/**
	* @brief The one frame clock (cleanup plan A10, B12), a static facade over CS's org::async::FrameClock. RenderGraphRuntime owns
	* it and moves it. Everyone else only reads it.
	*
	* - beginFrame moves at each Renderer::Begin, before the frame driver's OnBegin, so Begin reads the frame it starts. 0 before
	*   the first Begin.
	* - slot is beginFrame's frame slot: it selects the published copy and the latch regions.
	* - presentSerial is the present the render thread's work goes to. It starts at 1 and moves at each sealed present.
	* - gpuCompleted is the newest beginFrame whose graph work the GPU has finished. 0 before the first.
	*
	* Counters of other things keep their own names. "Generation" is for registrations only. Any thread.
	*/
	class FrameClock
	{
	public:
		/** @brief Frame slots: the frames the graph's work may run behind the render thread (RenderGraphRuntime::kGameFramesInFlight). */
		static constexpr std::uint32_t kSlots = 4;

		using Reading = org::async::FrameClock::Reading;

		static std::uint64_t BeginFrame() { return clock.BeginFrame(); }
		static std::uint32_t Slot() { return clock.Slot(); }
		static std::uint32_t SlotOf(std::uint64_t a_frame) { return static_cast<std::uint32_t>(a_frame % kSlots); }
		static std::uint64_t PresentSerial() { return clock.PresentSerial(); }
		static std::uint64_t GpuCompleted() { return clock.GpuCompleted(); }

		/** @brief All four. Each is read on its own, so they may straddle a move on another thread. */
		static Reading Read() { return clock.Read(); }

	private:
		friend class ::RenderGraphRuntime;

		static inline org::async::FrameClock clock{ kSlots };
	};
}

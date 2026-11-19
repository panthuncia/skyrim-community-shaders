#pragma once

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>


namespace DCLF::Reports
{
	/**
	 * @brief The report schedule. Every module's report lines cover the same window of kInterval frames.
	 *
	 * The render thread reports at Present once per frame number (Due). The coordinator reports from its pass, which may skip frame
	 * numbers (Crossing). A module with no frame number counts its calls (Window).
	 * The window is kInterval while any parity or diagnostic switch is set, and kQuietInterval in normal play, where the always-on
	 * lines would grow a player's log by about 100 MB an hour.
	 */
	constexpr std::uint32_t kInterval = 300;
	constexpr std::uint32_t kQuietInterval = 18000;

	/** @brief The report window in frames. Read once, after the features registered their switches. */
	inline std::uint32_t Interval()
	{
		static const std::uint32_t interval = AnyObserverSwitch() ? kInterval : kQuietInterval;
		return interval;
	}

	/** @brief True while a diagnostic capture records. The reports then log full statistics, as under CS_DCLF_STATS. */
	inline std::atomic<bool> captureStatistics{ false };

	/** @brief Whether reports log full statistics. Counters kept only under CS_DCLF_STATS stay empty during a capture. */
	inline bool FullStatistics() { return Diagnostics::Observe().stats || captureStatistics.load(std::memory_order_relaxed); }

	/**
	 * @brief Flushes asked for so far. A flush ends the current window early on every clock.
	 * A diagnostic capture flushes at its start and end, so one window covers exactly the capture.
	 */
	inline std::atomic<std::uint32_t> flushGeneration{ 0 };

	/** @brief Ends the current report window on every clock. Any thread. */
	inline void Flush() { flushGeneration.fetch_add(1, std::memory_order_acq_rel); }

	/** @brief True once per clock after each Flush. a_seen holds the generation that clock last saw. */
	inline bool TakeFlush(std::uint32_t& a_seen)
	{
		const auto now = flushGeneration.load(std::memory_order_acquire);
		if (now == a_seen)
			return false;
		a_seen = now;
		return true;
	}

	/** @brief The render thread's last seen flush. */
	inline std::uint32_t renderThreadFlushSeen = 0;

	/** @brief The render thread's clock. Call once per frame. */
	inline bool Due(std::uint32_t a_frame)
	{
		// Take the flush once a frame, so every Due call of that frame agrees.
		static std::uint32_t frameOfFlush = ~0u;
		if (TakeFlush(renderThreadFlushSeen))
			frameOfFlush = a_frame;
		return a_frame % Interval() == 0 || a_frame == frameOfFlush;
	}

	/** @brief A clock that may skip frame numbers. Due when a_frame is in a later window than the last call's. */
	struct Crossing
	{
		std::uint32_t window = 0;
		std::uint32_t flushSeen = 0;

		bool Due(std::uint32_t a_frame)
		{
			const bool flushed = TakeFlush(flushSeen);
			const std::uint32_t now = a_frame / Interval();
			if (now == window && !flushed)
				return false;
			window = now;
			return true;
		}
	};

	/** @brief A module's own call count. Due on every kInterval-th Tick. */
	struct Window
	{
		std::uint32_t count = 0;
		std::uint32_t flushSeen = 0;

		bool Tick()
		{
			const bool flushed = TakeFlush(flushSeen);
			if (++count < Interval() && !flushed)
				return false;
			count = 0;
			return true;
		}
	};

	/** @brief Milliseconds per QueryPerformanceCounter tick. */
	inline double MsPerTick()
	{
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		return 1000.0 / static_cast<double>(frequency.QuadPart);
	}

	/** @brief The frame's report lines. Render thread, Present, while DCLF runs. */
	void RenderThread(std::uint32_t a_frame);

	/** @brief The main camera's native passes since the last report, and set members among them, which must be 0. */
	struct LeakStats
	{
		std::uint64_t offered = 0, leakedInDepth = 0, leakedInOpaque = 0;
		std::vector<std::string> samples;  // the first few leaks
	};
	/** @brief The leak counts. Render thread. */
	LeakStats& Leaks();
}

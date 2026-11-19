#pragma once

// The engine's writers inside the engine-read window, kept out of DCLF's reads there.

#include <cstdint>

namespace DCLF::WindowWriters
{
	/**
	 * @brief Hooks two engine jobs that write the scene graph while the engine-read window is open.
	 *
	 * The cell animations job (AE FUN_1406d29d0, SE FUN_140640300) runs beside Main::Draw once the shadow cull is done, and writes animated
	 * references' transforms and effect values while FrameValues samples them. The "Animated cell object update" job (AE FUN_1406d3770,
	 * SE FUN_140640fc0) writes Havok node transforms the same way. Each thunk waits for the frame's producer to finish sampling, then
	 * suspends the window around the update. The producer waits for nothing of the engine's, so this cannot deadlock.
	 * Each thunk chains onto what its call holds.
	 */
	void Install();
	/** @brief Checks Install's three calls are CALL rel32s, before any hook is written. */
	void Preflight();

	/**
	 * @brief Render thread, CS_DCLF_PERSISTENT_PARITY. Holds the frame's updates after sampling until the parity check is done.
	 * Present releases it whatever happened.
	 */
	void Hold();
	void Release();

	/** @brief Each thunk's runs since the last Take and how long they waited for sampling, in microseconds. */
	struct Counters
	{
		struct Job
		{
			std::uint64_t runs = 0, waited = 0, waitedUs = 0, waitedMaxUs = 0;
		};
		Job cellAnimations, animatedObjects;
	};
	Counters Take();
	bool Installed();
}

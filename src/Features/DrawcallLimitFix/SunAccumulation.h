#pragma once

#include <atomic>
#include <cstdint>

namespace DCLF
{
	/**
	 * @brief The engine's own work for the sun's cascades, taken away where DCLF already draws the result
	 * (docs/development/skyrim-engine-notes.md, "The sun's accumulation"). AE only.
	 *
	 * M1, registration-free accumulation (toggle `skipSunAccumulation`, CS_DCLF_SUN_SKIP): the engine still culls
	 * the sun's cascades, but a geometry DCLF claims for the sun's views is not registered. BSShadowDirectionalLight::
	 * Accumulate culls each cascade and hands every geometry it reaches to the accumulator's registration
	 * (FUN_1414b2140, called from FUN_140e28af0). That builds the geometry's shadow passes, which PassCapture then
	 * withholds one by one, and ORs the cascade's bit into the property's activeLightMask, which the main pass reads.
	 * For a claimed geometry the thunks on those calls replicate the registration's early-outs and its mask write,
	 * and skip the passes. Everything else calls the original, so an unclaimed caster is registered and drawn by
	 * the engine as before, and no frame-wide verdict is needed.
	 *
	 * The timing probe (TEMP, CS_DCLF_SUN_TIMING=1) measures the render thread in the full-frustum cull
	 * (FUN_141511f30), in Accumulate, and in the sun's registrations.
	 */
	class SunAccumulation
	{
	public:
		static SunAccumulation& Get();

		/** @brief AE: the Accumulate vtable slot and the call sites; verified before patching. */
		void Install();
		bool Installed() const { return installed; }

		/** @brief Every kReportInterval frames: the counters and, with the timing probe, the times. */
		void Report(std::uint32_t a_frame, std::uint32_t a_interval);

		struct Stats
		{
			std::uint32_t frames = 0;         // Accumulate calls for the sun
			std::uint32_t activeFrames = 0;   // ... with M1 active
			std::uint64_t skipped = 0;        // claimed geometries not registered (the mask written)
			std::uint64_t registered = 0;     // sun registrations passed to the engine
			std::uint64_t offThread = 0;      // sun registrations outside the Accumulate call (passed to the engine)
			std::int64_t fullFrustumTicks = 0, fullFrustumMax = 0;
			std::int64_t accumulateTicks = 0, accumulateMax = 0;
			std::int64_t registrationTicks = 0;  // inside Accumulate, the registrations (timing probe only)
		};

	private:
		SunAccumulation() = default;

		struct Hooks;
		friend struct Hooks;

		bool installed = false;
		Stats stats;
		std::atomic<std::uint64_t> offThread{ 0 };
	};
}

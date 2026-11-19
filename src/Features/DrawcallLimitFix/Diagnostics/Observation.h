#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Diagnostics
{
	/**
	 * @brief Which subjects this run observes, from the CS_DCLF_* switches.
	 * Read once before the first frame, so nothing changes under a running frame. Observers that change what the main path does
	 * are Policy.h questions instead.
	 */
	struct Observation
	{
		// Parity checks compare a live path against a reference built from scratch and flag each mismatch with "<-".
		bool scene = false;          // CS_DCLF_PERSISTENT_PARITY, every kept store against a rebuild
		bool claims = false;         // CS_DCLF_SET_PARITY, claims against engine registrations and GPU drawn bits against the builds
		bool claimsOrScene = false;  // either of the two, for the observers they share
		bool walk = false;           // CS_DCLF_WALK_PARITY, the delta walk against a dense walk
		bool walkOrScene = false;    // either of the two, for the hidden witness
		bool changeLog = false;      // CS_DCLF_CHANGE_LOG_PARITY, the change log against a diff of the tables
		bool residents = false;      // CS_DCLF_RESIDENT_PARITY, resident records against their passes built again
		bool builds = false;         // CS_DCLF_BUILD_PARITY, BuildDraws' output and index ranges against the CPU's
		bool mirror = false;         // CS_DCLF_MIRROR_PARITY, the mirror's reads against the live objects
		bool capture = false;        // CS_DCLF_CAPTURE_PARITY, the tables against the engine's own lighting draws
		bool engineDrawsAll = false; // CS_DCLF_PARITY_BOTH, nothing is withheld from the engine
		bool capturePoint = false;   // CS_DCLF_CAPTURE_POINT_PARITY, captured bindings against the first native lighting draw
		bool accumulated = false;    // CS_DCLF_ACCUMULATED_PARITY, decoded visibility and the animation gate against the engine's
		bool foliage = false;        // CS_DCLF_FOLIAGE_PARITY=1|wind|land, foliage pixels against the frame before
		bool foliageFrozen = false;  // ... with tree wind frozen, for 1 or land
		bool foliageLand = false;    // ... and every colour draw records its pixels, for land

		// Statistics, probes and watches.
		bool stats = false;            // CS_DCLF_STATS, the periodic report
		bool passStats = false;        // CS_DCLF_PASS_STATS, per-pass counts
		bool profile = false;          // CS_DCLF_PROFILE, scene-phase timings
		bool releaseGuard = false;     // CS_DCLF_RELEASE_GUARD, engine releases on DCLF's threads
		bool frameTrace = false;       // CS_DCLF_FRAME_TRACE, per-frame adoption, coverage, epochs, culling and retirement, for corrupt frames
		std::string targetProbe;       // CS_DCLF_TARGET_PROBE=x,y
	};

	/** @brief The run's observation, read from the switches on first use. */
	const Observation& Observe();

	/** @brief Parity checks run every 60 frames or passes, each at its own offset so their rebuilds do not share a frame. */
	constexpr bool ParityDue(std::uint64_t a_serial, std::uint32_t a_offset = 0) { return a_serial % 60 == a_offset; }
}

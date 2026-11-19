#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Core/Engine/FrameAddressSet.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/SunExclusion.h"

namespace DCLF::Features::Shadows::Sun
{
	struct SunAccumulationView;

	/**
	 * @brief Removes the engine's sun cascade work where DCLF already draws the result. Render thread, plus job-thread thunks.
	 *
	 * Three levels, all on while DCLF draws the shadows. Registration skip: for a DCLF member the registration thunks only write
	 * the cascade mask bit and build no shadow passes. Entry exclusion: entries DCLF draws are removed from the full-frustum
	 * object list, and DCLF writes their sun bits at the main registration. Whole skip: when DCLF draws every cascade, the
	 * full-frustum cull gets an empty list. A caster that is not yet a member casts no sun shadow until it joins.
	 */
	class SunAccumulation
	{
	public:
		static SunAccumulation& Get();
		/** @brief Whether this is the intentionally empty scene list array for DCLF-owned sun cascades. */
		static bool IsWholeCullInput(const void* a_lists);

		/** @brief Patches the Accumulate vtable slot and call sites, after verifying them. */
		void Install();
		/**
		 * @brief Checks every site before any hook is written.
		 * Most of these hooks replace the engine's call rather than chain, so each needs the engine's own code there.
		 */
		static void Preflight();
		bool Installed() const { return installed; }
		/** @brief Reads the toggles. Main thread, before DCLF's threads start. */


		/** @brief Whether this frame's full-frustum cull applied the entry exclusion (or the whole skip). */
		bool ExclusionLive() const { return exclusionLive.load(std::memory_order_acquire); }
		/** @brief On a frame the full-frustum cull did not run, clears cascades, exclusion and sun bits. Render thread. */
		void EndFullFrustumWindow();
		/** @brief Whether a_node is an entry this frame's exclusion excludes (render thread, while it is live). */
		bool Excluded(const void* a_node) const
		{
			const auto* exclusion = frameState.exclusion.get();
			if (!exclusion || !exclusion->candidates)
				return false;
			const auto it = exclusion->candidates->entries.find(static_cast<const RE::NiAVObject*>(a_node));
			return it != exclusion->candidates->entries.end() && exclusion->excluded[it->second];
		}


		struct Stats
		{
			std::uint32_t frames = 0;         // Accumulate calls for the sun
			std::uint32_t activeFrames = 0;   // ... with the registration skip active
			std::uint64_t skipped = 0;        // claimed geometries not registered (the mask written)
			std::uint64_t registered = 0;     // sun registrations passed to the engine
			std::uint64_t offThread = 0;      // sun registrations outside Accumulate (passed to the engine)
			std::int64_t fullFrustumTicks = 0, fullFrustumMax = 0;
			std::int64_t accumulateTicks = 0, accumulateMax = 0;
			std::uint32_t exclusionFrames = 0;        // full-frustum culls that applied one
			std::uint32_t exclusionStale = 0;         // ... built for other candidates
			std::uint32_t exclusionMissing = 0;       // ... with none published
			std::uint32_t exclusionForwarded = 0;     // frame starts that took the builder's translation (SunExclusionSlot)
			std::uint32_t exclusionUnforwarded = 0;   // ... that found none for the frame's candidates
			std::uint64_t entriesSeen = 0;            // objectArray entries the applied culls found
			std::uint64_t entriesRemoved = 0;         // ... and took out
			std::uint64_t candidates = 0;             // candidates, and excluded ones, of the applied exclusions
			std::uint64_t excluded = 0;
			std::int64_t filterTicks = 0;             // the render thread's compaction
			std::uint64_t cascadeRegistrationsUnderRemoved = 0;  // must be 0
			// Whole-skip frames, and frames with the exclusion on where it could not apply.
			std::uint32_t wholeFrames = 0;
			std::uint32_t wholeDeclined = 0;
			std::uint32_t wholeHeld = 0;  // whole frames armed though not covered: a load's hold's (Core/Render/LoadHold.h)
			std::uint32_t wholeTargeted = 0;
			std::uint64_t wholeTargetedRoots = 0;
			// Full-frustum processes each whole-skip cascade culled, and those left out.
			std::uint64_t wholeProcessesCulled = 0;
			std::uint64_t wholeProcessesLeft = 0;
		};
		/** @brief Excluded entries the cascade culls still reached through a node above. Counted on job threads. */
		struct SkipStats
		{
			std::atomic<std::uint64_t> skipped{ 0 };
		};
		SkipStats skipStats;

		/** @brief The registration thunks' view of the sun's bits (job threads). */
		struct BitStats
		{
			std::atomic<std::uint64_t> written{ 0 };       // removed-entry registrations that took DCLF's bits
			std::atomic<std::uint64_t> withBits{ 0 };      // ... with at least one cascade's bit
			std::atomic<std::uint64_t> notReady{ 0 };      // ... before the cascades were known (must be 0)
			// Whole-skip main registrations that took DCLF's bits. The overflow count must stay 0.
			std::atomic<std::uint64_t> wholeWritten{ 0 };
			std::atomic<std::uint64_t> wholeWithBits{ 0 };
			std::atomic<std::uint64_t> wholeUnstamped{ 0 };
		};

		/** @brief Returns and resets the counters. Render thread. */
		Stats TakeStats();
		/** @brief The entry exclusion's registration and skip counts since the last call, reset. */
		struct ExclusionCounts
		{
			std::uint64_t written = 0, withBits = 0, notReady = 0, skipped = 0;
			std::uint64_t wholeWritten = 0, wholeWithBits = 0, wholeUnstamped = 0;
		};
		ExclusionCounts TakeExclusionCounts();
		/** @brief Whether this frame's full-frustum cull took the whole skip. Render thread. */
		bool WholeFrame() const { return frameState.whole; }

	private:
		SunAccumulation() = default;

		struct Hooks;
		friend struct Hooks;
		friend struct SunAccumulationView;

		/** @brief Arms the veto that keeps excluded entries out of the full-frustum object lists. Render thread, before the cull. */
		void ArmExclusion(RE::BSShadowDirectionalLight* a_light);
		/**
		 * @brief Disarms the veto and makes its result the frame's exclusion. Render thread, after the cull.
		 * Each process keeps at least one entry, because the cascade cull sets its planes up from the first.
		 */
		void ExcludeEntries();
		/** @brief Whether the frame takes the whole skip, and if so sets the frame's state. Render thread, before the cull. */
		bool ArmWhole(RE::BSShadowDirectionalLight* a_light);
		/** @brief Marks the frame's exclusion live after the whole skip's cull. Render thread. */
		void WholeApplied();
		/** @brief Whether a_geometry's first clearing registration (0xFFFF) was seen this frame. a_clears marks it. Any thread. */
		bool ClearedThisFrame(const void* a_geometry, bool a_clears);
		/** @brief The candidate geometry index if under an entry this frame removed, else ~0u. Any thread. */
		std::uint32_t RemovedGeometryIndex(const RE::BSGeometry* a_geometry) const;
		bool UnderRemovedEntry(const RE::BSGeometry* a_geometry) const { return RemovedGeometryIndex(a_geometry) != ~0u; }
		/** @brief Whether a_geometry is under an entry this frame's exclusion excludes (removed or skipped). */
		bool UnderExcludedEntry(const RE::BSGeometry* a_geometry) const
		{
			const auto* exclusion = frameState.exclusion.get();
			if (!exclusion || !exclusion->candidates)
				return false;
			// Any ancestor counts, as CascadeProcess1 skips it. A caster under an excluded entry need not be a candidate geometry.
			const auto& entries = exclusion->candidates->entries;
			for (const RE::NiAVObject* object = a_geometry; object; object = object->parent)
				if (const auto it = entries.find(object); it != entries.end() && exclusion->excluded[it->second])
					return true;
			return false;
		}
		/** @brief ORs the bits of the cascades a removed-entry geometry's bound meets into its activeLightMask. Any thread. */
		void ApplySunBits(RE::BSGeometry* a_geometry, bool a_clears);

		bool installed = false;
		Stats stats;
		std::atomic<std::uint64_t> offThread{ 0 };

		/** @brief One cascade's cull volume and bit, as the engine's cascade cull used them this frame. */
		struct Cascade
		{
			std::array<std::array<float, 4>, 6> planes{};  // normal, constant: outside when n.c - d < -r
			std::uint32_t planeMask = 0;
			std::array<std::array<float, 4>, 6> customPlanes{};
			std::uint32_t customMask = 0;  // 0: no custom planes
			std::uint32_t bit = 0;         // the accumulator's +0x164
			bool captured = false;         // DCLF's volume is known (the Sun board)
		};

		/** @brief The frame's exclusion, as the registration thunks read it. */
		struct FrameState
		{
			std::shared_ptr<Local::SunExclusion> exclusion;  // kept alive until the next full-frustum cull
			std::uint32_t stamp = 0;
			bool skipExcluded = true;
			// Grown in the sun's Accumulate while bitsReady is clear, so no reader sees it move.
			std::vector<Cascade> cascades;
			std::uint32_t cascadeCount = 0;
			std::uint32_t cascadesFrame = ~0u;        // the scene frame whose Accumulate captured them
			std::uint32_t sunBits = 0;                // every cascade's bit
			bool whole = false;                       // the whole skip: every main registration takes DCLF's bits
		};
		std::shared_ptr<Local::SunExclusion> previousExclusion;  // the last frame's, kept alive for a straggling registration
		/**
		 * @brief One full-frustum process while the veto is armed.
		 * Only one job appends to it at a time, and the render thread reads the counts after the cull.
		 */
		struct VetoProcess
		{
			const void* process = nullptr;
			std::uint32_t seen = 0, vetoed = 0;
			RE::NiAVObject* firstVetoed = nullptr;  // given back when nothing else was appended
			std::uint32_t firstEntry = 0;
			bool givenBack = false;
		};
		std::vector<VetoProcess> vetoProcesses;          // written on the render thread before the veto is armed
		std::shared_ptr<Local::SunExclusion> armedExclusion;     // render thread: the exclusion the armed veto applies
		std::atomic<const Local::SunExclusion*> vetoExclusion{ nullptr };  // the appends' view of it (null: disarmed)
		std::uint32_t vetoStamp = 0;                      // written before the veto is armed
		FrameState frameState;                           // written on the render thread before bitsReady
		// Set at the end of the sun's Accumulate. Cleared when Main::Draw's mask clear returns and at the full-frustum cull.
		std::atomic<bool> bitsReady{ false };
		std::atomic<bool> exclusionLive{ false };        // frameState.exclusion is set for this frame
		bool fullFrustumRan = false;                     // render thread: since EndFullFrustumWindow
		void ResetFrameSun();
		std::atomic<std::uint32_t> stampCounter{ 0 };
		BitStats bitStats;
		// Geometries whose clearing registration took DCLF's bits this frame.
		FrameAddressSet wholeCleared;
	};
}

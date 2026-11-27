#pragma once

// Entry exclusions for the sun's cascades and the paraboloid lights, their translations to later candidates, and the slots they
// wait in until the next full-frustum cull or point-light culls take them. Core holds them only opaquely.

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Scene/EntrySpace.h"

namespace DCLF::Features::Shadows::Local
{
	/**
	 * @brief One shadow epoch's verdict: the entries whose casters it fully drew, left out of the next frame's cascade culls.
	 * Immutable once published, except its stamped words and forward link. Republished unchanged while nothing moved.
	 */
	struct SunExclusion
	{
		SunExclusion() = default;
		SunExclusion(const SunExclusion&) = delete;
		SunExclusion& operator=(const SunExclusion&) = delete;

		std::shared_ptr<const EntrySpace> candidates;
		std::vector<std::uint8_t> excluded;  // per candidate index
		std::uint32_t excludedCount = 0;
		// Changes only when excluded does (Api::ShadowListExclusion::version).
		std::uint64_t version = 0;
		// Stamped words are shared by one mode's versions and never reset. Each holds the stamp of the frame that wrote it, and only one
		// version per mode is stamped a frame, so another version's write never reads as this frame's.
		std::shared_ptr<std::atomic<std::uint32_t>[]> removed;
		// Per geometry, the frame whose main registration cleared its mask, so DCLF's sun bits are not written again that frame.
		std::shared_ptr<std::atomic<std::uint32_t>[]> cleared;
		// Paraboloid mode only: per candidate, three words of the point lights whose cull reached and skipped the entry this frame.
		std::shared_ptr<std::atomic<std::uint64_t>[]> lightReach;
		std::uint32_t removedCount = 0;  // entries removed and lightReach (times 3) hold
		std::uint32_t clearedCount = 0;  // geometries cleared holds
		// The scene frame of the shadow build it came from, and whether it reused that cache's verdict (diagnostics).
		std::uint32_t builtFrame = 0;
		bool reused = false;

		// This verdict translated to the next candidates. Written once (forwardOwned, then forward released), read by any thread after.
		std::shared_ptr<SunExclusion> forwardOwned;
		std::atomic<const SunExclusion*> forward{ nullptr };
		// Entries changed between the predecessor's candidates and this one's. They are not excluded.
		std::vector<std::uint32_t> forwardChanged;
	};

	/**
	 * @brief Translates a_exclusion to a_to: changed entries are not excluded, the rest keep their verdict.
	 * The builder makes these. A reader translating itself pays O(changes + candidates).
	 */
	std::shared_ptr<SunExclusion> TranslateExclusion(const std::shared_ptr<SunExclusion>& a_exclusion, const std::shared_ptr<const EntrySpace>& a_to,
		bool a_newVersion, bool a_lightReach, std::vector<std::uint32_t>* a_changed = nullptr, const SunExclusion* a_stamps = nullptr);

	/** @brief a_exclusion for a_to by following the builder's forward links, or null if it made none. O(links), any thread. */
	std::shared_ptr<SunExclusion> ForwardExclusion(const std::shared_ptr<SunExclusion>& a_exclusion, const std::shared_ptr<const EntrySpace>& a_to,
		std::vector<std::uint32_t>* a_changed = nullptr);

	/**
	 * @brief Holds the sun's exclusion from the shadow epoch until the full-frustum cull applies it once. The render side publishes,
	 * native's game half forwards and takes it: every access is an atomic exchange of the pointer.
	 */
	class SunExclusionSlot
	{
	public:
		static SunExclusionSlot& Get();

		/** @brief Sets the exclusion the next full-frustum cull applies once. Called after the shadow epoch. */
		void Publish(std::shared_ptr<SunExclusion> a_exclusion) { std::atomic_store(&pending, std::move(a_exclusion)); }
		/** @brief The exclusion the next full-frustum cull will apply, or null. */
		std::shared_ptr<const SunExclusion> Pending() const { return std::atomic_load(&pending); }
		/** @brief Takes the pending exclusion and leaves the slot empty. */
		std::shared_ptr<SunExclusion> Take() { return std::atomic_exchange(&pending, std::shared_ptr<SunExclusion>{}); }
		/**
		 * @brief At frame start, swaps the pending exclusion for the builder's translation to a_candidates.
		 * With none, the pending one stays and the full-frustum cull treats it as stale.
		 */
		void BeginFrame(const std::shared_ptr<const EntrySpace>& a_candidates);

		/** @brief Frame starts that found the builder's translation, and those that did not. */
		struct ForwardCounts
		{
			std::uint32_t forwarded = 0;
			std::uint32_t unforwarded = 0;
		};
		/** @brief Returns and resets the counts. */
		ForwardCounts TakeForwardCounts() { return std::exchange(counts, {}); }

	private:
		SunExclusionSlot() = default;

		std::shared_ptr<SunExclusion> pending;
		ForwardCounts counts;
	};

	/**
	 * @brief Holds the paraboloid exclusion until the next frame's point-light culls take it.
	 * Published only while the Point source's culls are installed. The render side publishes, native's game half reads: atomic.
	 */
	class ParabolicExclusionSlot
	{
	public:
		static ParabolicExclusionSlot& Get();

		/** @brief Sets next frame's exclusion. Called after the shadow epoch. */
		void Publish(std::shared_ptr<SunExclusion> a_exclusion) { std::atomic_store(&pending, std::move(a_exclusion)); }
		/** @brief The last published exclusion, or null. It stays until the next Publish. */
		std::shared_ptr<SunExclusion> Pending() const { return std::atomic_load(&pending); }

	private:
		ParabolicExclusionSlot() = default;

		std::shared_ptr<SunExclusion> pending;
	};
}

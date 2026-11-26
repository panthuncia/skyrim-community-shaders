#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace DCLF
{
	/**
	 * @brief When a worker may touch the engine's scene graph: the game-side capture window (roadmap phase 6).
	 *
	 * It opens on the main thread once the game update published its snapshot and closes before the next update's game logic, at the
	 * end of Main::Update's call of Main::DrawFrame. The render side never takes a lease: it reads publications. A worker takes a Lease
	 * per item or bounded chunk, and a refused lease means the window closed, so it leaves the rest for the next window. Close waits for
	 * leases in flight, so the game logic after it never runs beside a worker in engine memory. One atomic word holds the open bit, the
	 * render-half hold, the writers' suspensions and the lease count, so no lease can be taken after Close has seen the count.
	 *
	 * While the engine's render half still runs inside the window, it writes the scene graph outside Main::Draw to Present (the decal
	 * manager's attaches before Main::Draw, the next update's jobs after Present). The window is held there (HoldForRenderHalf), so
	 * leases are taken only from Main::Draw to Present, as before. Without a render half (a dropped cadence update, phase 7) the whole
	 * window is readable.
	 */
	class EngineReadWindow
	{
	public:
		/** @brief Main thread, game side, after the update's publish. The engine's update is done, so workers may read. */
		static void Open();
		/**
		 * @brief Main thread, game side, before the next update's game logic. Refuses new leases and returns once the leases in flight are
		 * released. Ends a render-half hold.
		 */
		static void Close();
		/** @brief Whether the window is open and not held by the render half, suspended by a writer or not. */
		static bool IsOpen() { return (state.load(std::memory_order_acquire) & (kOpen | kHeld)) == kOpen; }
		/**
		 * @brief Main thread, inside the update's render half: the engine writes its scene graph from here (DrawFrame's start, Present).
		 * Refuses new leases until ReleaseRenderHalf and waits for those in flight. Does nothing when already held, outside an update's
		 * render half or off the main thread.
		 */
		static void HoldForRenderHalf();
		/** @brief Main thread, at Main::Draw inside the update's render half: the engine only reads its scene graph until Present. */
		static void ReleaseRenderHalf();
		/**
		 * @brief An engine writer's thread, inside the window. Refuses new leases until Resume and waits for those in flight. Writers may
		 * overlap, and the window resumes at the last Resume. A Close meanwhile still closes it.
		 */
		static void Suspend();
		static void Resume();

		/** @brief One item's access to engine memory. False means the window is closed and the item must not be done. */
		class Lease
		{
		public:
			Lease();
			~Lease();
			Lease(Lease&& a_other) noexcept :
				held(std::exchange(a_other.held, false)) {}
			Lease(const Lease&) = delete;
			Lease& operator=(const Lease&) = delete;
			Lease& operator=(Lease&&) = delete;
			explicit operator bool() const { return held; }

		private:
			bool held = false;
		};

		/** @brief Set on threads running DCLF's scene work. */
		static inline thread_local bool sceneWork = false;
		/** @brief An access to engine memory, counted as a defect when a scene work thread holds no lease. */
		static void Touch(const char* a_site)
		{
			if (sceneWork && !heldHere)
				NoteUnleased(a_site);
		}

		/** @brief The report's counters since the last Take. */
		struct Counters
		{
			std::uint64_t closes = 0, holds = 0, closesWaited = 0, suspends = 0, suspendsWaited = 0, refused = 0, unleased = 0, mainThreadLeases = 0;
			double waitedUs = 0.0, waitedMaxUs = 0.0, suspendWaitedUs = 0.0, suspendWaitedMaxUs = 0.0;
			const char* unleasedFirst = nullptr;
			bool any = false;  // a close or an unleased access since the last Take
		};
		/** @brief Main thread. Takes and resets the counters. */
		static Counters Take();

	private:
		static void NoteUnleased(const char* a_site);
		/** @brief Spins until no lease is in flight. Returns the microseconds it waited. */
		static double DrainLeases();

		// The open bit, the render half's hold, the count of writers suspending it, and the leases in flight below. A lease is taken only
		// while open, not held and not suspended.
		static constexpr std::uint32_t kOpen = 1u << 31;
		static constexpr std::uint32_t kHeld = 1u << 30;
		static constexpr std::uint32_t kSuspendUnit = 1u << 24;
		static constexpr std::uint32_t kSuspended = kHeld - kSuspendUnit;
		static constexpr std::uint32_t kLeases = kSuspendUnit - 1;
		/** @brief Counts a wait at Close or a hold. */
		static void NoteBoundary(std::uint32_t a_before, bool a_hold);
		static inline thread_local std::uint32_t heldHere = 0;  // the leases this thread holds
		static inline std::atomic<std::uint64_t> unleased{ 0 };
		static inline std::atomic<const char*> unleasedFirst{ nullptr };
		static inline std::atomic<std::uint32_t> state{ 0 };
		static inline std::atomic<std::uint64_t> refused{ 0 };
		// Leases taken on the main thread, which is the render side inside the window. Any is a defect: the render side reads publications.
		static inline std::atomic<std::uint64_t> mainThreadLeases{ 0 };
		// Main thread.
		static inline bool renderHalfHeld = false;
		static inline std::uint64_t closes = 0, holds = 0, closesWaited = 0;
		static inline double waitedUs = 0.0, waitedMaxUs = 0.0;
		// The writer's thread. Waits are in whole microseconds.
		static inline std::atomic<std::uint64_t> suspends{ 0 }, suspendsWaited{ 0 }, suspendWaitedUs{ 0 }, suspendWaitedMaxUs{ 0 };
	};
}

// The release guard is a diagnostic, included here until its callers include it.
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ReleaseGuard.h"

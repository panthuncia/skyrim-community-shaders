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
	 * It is open while the engine only reads its scene graph: from DCLF's game half (FrameDriver::GameFrameStart, at Main::Draw's
	 * SetupSunAndSky, or a dropped update's game half) to the start of the engine's next update jobs: guest's Present (CloseAtPresent),
	 * native's end of the update's renders, or a routed update's end of its game frame right before the publish, before CS raises the job
	 * stages (Close), and at the latest the end of Main::Update's call of Main::DrawFrame (Close). Before the game half, DrawFrame writes
	 * the scene graph (the decal manager's attaches). The render side never takes a lease: it reads publications and DCLF's ticket. A worker takes a
	 * Lease per item or bounded chunk, and a refused lease means the window closed, so it leaves the rest for the next window. Closing
	 * waits for leases in flight, so the engine writes after it never run beside a worker in engine memory. One atomic word holds the
	 * open bit, the writers' suspensions and the lease count, so no lease can be taken after a close has seen the count.
	 */
	class EngineReadWindow
	{
	public:
		/** @brief Main thread, DCLF's game half: the engine only reads its scene graph from here, so workers may read. */
		static void Open();
		/**
		 * @brief Main thread, game side, before the next update's game logic. Refuses new leases and returns once the leases in flight are
		 * released.
		 */
		static void Close();
		/**
		 * @brief Guest's Present: the engine's next update jobs start here. Closes as Close does, but only inside a game update (a
		 * loading screen's Present on the serving thread is outside one). Native closes at the end of the update's renders instead.
		 */
		static void CloseAtPresent();
		/** @brief Whether the window is open, suspended by a writer or not. */
		static bool IsOpen() { return (state.load(std::memory_order_acquire) & kOpen) == kOpen; }
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
		/** @brief Marks the thread as running a job the render side kicked. A lease asked for on it is counted: the render side takes none. */
		class RenderSideJob
		{
		public:
			RenderSideJob() :
				outer(std::exchange(renderSideJob, true)) {}
			~RenderSideJob() { renderSideJob = outer; }
			RenderSideJob(const RenderSideJob&) = delete;
			RenderSideJob& operator=(const RenderSideJob&) = delete;

		private:
			bool outer;
		};
		/** @brief An access to engine memory, counted as a defect when a scene work thread holds no lease. */
		static void Touch(const char* a_site)
		{
			if (sceneWork && !heldHere)
				NoteUnleased(a_site);
		}

		/** @brief The report's counters since the last Take. */
		struct Counters
		{
			std::uint64_t closes = 0, closesWaited = 0, suspends = 0, suspendsWaited = 0, refused = 0, unleased = 0, mainThreadLeases = 0, renderSideJobLeases = 0;
			double waitedUs = 0.0, waitedMaxUs = 0.0, suspendWaitedUs = 0.0, suspendWaitedMaxUs = 0.0;
			const char* unleasedFirst = nullptr;
			bool any = false;  // a close or an unleased access since the last Take
		};
		/** @brief Any thread (the reports). Takes and resets the counters. */
		static Counters Take();

	private:
		static void NoteUnleased(const char* a_site);
		/** @brief Spins until no lease is in flight. Returns the microseconds it waited. */
		static double DrainLeases();

		// The open bit, the count of writers suspending it, and the leases in flight below. A lease is taken only while open and not
		// suspended.
		static constexpr std::uint32_t kOpen = 1u << 31;
		static constexpr std::uint32_t kSuspendUnit = 1u << 24;
		static constexpr std::uint32_t kSuspended = kOpen - kSuspendUnit;
		static constexpr std::uint32_t kLeases = kSuspendUnit - 1;
		/** @brief Counts a close and its wait. */
		static void NoteClose(std::uint32_t a_before);
		static inline thread_local std::uint32_t heldHere = 0;  // the leases this thread holds
		static inline std::atomic<std::uint64_t> unleased{ 0 };
		static inline std::atomic<const char*> unleasedFirst{ nullptr };
		static inline std::atomic<std::uint32_t> state{ 0 };
		static inline std::atomic<std::uint64_t> refused{ 0 };
		// Leases taken on the main thread, which is the render side inside the window. Any is a defect: the render side reads publications.
		static inline std::atomic<std::uint64_t> mainThreadLeases{ 0 };
		// Leases asked for on a thread inside a RenderSideJob. Any is a defect.
		static inline thread_local bool renderSideJob = false;
		static inline std::atomic<std::uint64_t> renderSideJobLeases{ 0 };
		// The closing thread adds, the reports take. Waits in nanoseconds.
		static inline std::atomic<std::uint64_t> closes{ 0 }, closesWaited{ 0 }, waitedNs{ 0 }, waitedMaxNs{ 0 };
		// The writer's thread. Waits are in whole microseconds.
		static inline std::atomic<std::uint64_t> suspends{ 0 }, suspendsWaited{ 0 }, suspendWaitedUs{ 0 }, suspendWaitedMaxUs{ 0 };
	};
}

// The release guard is a diagnostic, included here until its callers include it.
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ReleaseGuard.h"

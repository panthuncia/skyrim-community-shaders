#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace DCLF
{
	/**
	 * @brief When a worker may touch the engine's scene graph.
	 *
	 * From Main::Draw to Present the engine reads its scene graph but does not write it, apart from writers WindowWriters suspends the
	 * window for. A worker takes a Lease per item or bounded chunk, and a refused lease means the window closed, so it leaves the rest
	 * for the next window. Close waits for leases in flight, so the update after Present never runs beside a worker in engine memory.
	 * One atomic word holds the open bit and the lease count, so no lease can be taken after Close has seen the count.
	 */
	class EngineReadWindow
	{
	public:
		/** @brief Render thread, at Main::Draw. The engine's update is done, so workers may read. */
		static void Open();
		/** @brief Render thread, at Present. Refuses new leases and returns once the leases in flight are released. */
		static void Close();
		/** @brief Whether the frame is between Main::Draw and Present, suspended or not. */
		static bool IsOpen() { return (state.load(std::memory_order_acquire) & kOpen) != 0; }
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
			std::uint64_t closes = 0, closesWaited = 0, suspends = 0, suspendsWaited = 0, refused = 0, unleased = 0;
			double waitedUs = 0.0, waitedMaxUs = 0.0, suspendWaitedUs = 0.0, suspendWaitedMaxUs = 0.0;
			const char* unleasedFirst = nullptr;
			bool any = false;  // a close or an unleased access since the last Take
		};
		/** @brief Render thread. Takes and resets the counters. */
		static Counters Take();

	private:
		static void NoteUnleased(const char* a_site);
		/** @brief Spins until no lease is in flight. Returns the microseconds it waited. */
		static double DrainLeases();

		// The open bit, the count of writers suspending it, and the leases in flight below. A lease is taken only while open and not suspended.
		static constexpr std::uint32_t kOpen = 1u << 31;
		static constexpr std::uint32_t kSuspendUnit = 1u << 24;
		static constexpr std::uint32_t kSuspended = kOpen - kSuspendUnit;
		static constexpr std::uint32_t kLeases = kSuspendUnit - 1;
		static inline thread_local std::uint32_t heldHere = 0;  // the leases this thread holds
		static inline std::atomic<std::uint64_t> unleased{ 0 };
		static inline std::atomic<const char*> unleasedFirst{ nullptr };
		static inline std::atomic<std::uint32_t> state{ 0 };
		static inline std::atomic<std::uint64_t> refused{ 0 };
		// Render thread.
		static inline std::uint64_t closes = 0, closesWaited = 0;
		static inline double waitedUs = 0.0, waitedMaxUs = 0.0;
		// The writer's thread. Waits are in whole microseconds.
		static inline std::atomic<std::uint64_t> suspends{ 0 }, suspendsWaited{ 0 }, suspendWaitedUs{ 0 }, suspendWaitedMaxUs{ 0 };
	};
}

// The release guard is a diagnostic, included here until its callers include it.
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ReleaseGuard.h"

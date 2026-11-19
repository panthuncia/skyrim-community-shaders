// Runs the cell animation jobs' updates after the frame's sampling, with the engine-read window suspended.
#include "Features/DrawcallLimitFix/Core/Engine/WindowWriters.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"

#include <atomic>
#include <chrono>

namespace DCLF::WindowWriters
{
	namespace
	{
		bool installed = false;
		std::atomic<bool> held{ false };  // the updates wait for Release

		struct JobCounters
		{
			std::atomic<std::uint64_t> runs{ 0 }, waited{ 0 }, waitedUs{ 0 }, waitedMaxUs{ 0 };

			Counters::Job Take()
			{
				return { runs.exchange(0, std::memory_order_relaxed), waited.exchange(0, std::memory_order_relaxed), waitedUs.exchange(0, std::memory_order_relaxed),
					waitedMaxUs.exchange(0, std::memory_order_relaxed) };
			}
		};
		JobCounters cellAnimations, animatedObjects;

		// After the frame's sampling, and a parity frame's Release, with the window suspended until EndWriter.
		void BeginWriter(JobCounters& a_counters)
		{
			a_counters.runs.fetch_add(1, std::memory_order_relaxed);
			const auto start = std::chrono::steady_clock::now();
			FrameValues::Get().WaitSampled();
			held.wait(true, std::memory_order_acquire);
			if (const auto us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count())) {
				a_counters.waited.fetch_add(1, std::memory_order_relaxed);
				a_counters.waitedUs.fetch_add(us, std::memory_order_relaxed);
				for (auto max = a_counters.waitedMaxUs.load(std::memory_order_relaxed); us > max && !a_counters.waitedMaxUs.compare_exchange_weak(max, us, std::memory_order_relaxed);) {}
			}
			EngineReadWindow::Suspend();
		}

		void EndWriter() { EngineReadWindow::Resume(); }

		template <class F>
		void AsWriter(JobCounters& a_counters, F&& a_update)
		{
			BeginWriter(a_counters);
			a_update();
			EndWriter();
		}

		struct CellAnimations
		{
			// TES and the frame's animation time (xmm1).
			static void thunk(RE::TES* a_tes, float a_time)
			{
				AsWriter(cellAnimations, [&] { func(a_tes, a_time); });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The window is suspended from the job's queuing call to the end of its wait.
		struct AnimatedObjects
		{
			static void thunk(void* a_group)
			{
				BeginWriter(animatedObjects);
				func(a_group);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AnimatedObjectsWait
		{
			static void thunk(void* a_group)
			{
				func(a_group);
				EndWriter();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Chains onto whatever the call holds, and logs when that is not the engine's.
		template <class T>
		void Chain(const Engine::Offset& a_call, const Engine::Offset& a_target, const char* a_job)
		{
			const auto site = REL::Offset(a_call).address();
			if (!Engine::CallsTo(site, REL::Offset(a_target).address()))
				logger::info("[DCLF] the {} job's call is hooked by {}; DCLF's thunk chains onto it", a_job, HookSites::BranchTargetModule(site));
			stl::write_thunk_call<T>(site);
		}

		void Installed(const char* a_job)
		{
			logger::info("[DCLF] {} job: its update waits for the frame values' sampling, the engine-read window suspended around it", a_job);
		}
	}

	void Preflight()
	{
		HookSites::RequireCall("the cell animations job's update call", REL::Offset(Engine::Address::kCellAnimationsCall).address());
		HookSites::RequireCall("the animated cell object update's queuing call", REL::Offset(Engine::Address::kAnimatedObjectsCall).address());
		HookSites::RequireCall("the animated cell object update's wait call", REL::Offset(Engine::Address::kAnimatedObjectsWaitCall).address());
	}

	void Install()
	{
		if (installed)
			return;
		Chain<CellAnimations>(Engine::Address::kCellAnimationsCall, Engine::Address::kCellAnimationsUpdate, "cell animations");
		Installed("cell animations");
		// Both calls are required, because a suspension never resumed would refuse every lease after it.
		constexpr const char* kObjects = "animated cell object update";
		Chain<AnimatedObjects>(Engine::Address::kAnimatedObjectsCall, Engine::Address::kAnimatedObjectsUpdate, kObjects);
		Chain<AnimatedObjectsWait>(Engine::Address::kAnimatedObjectsWaitCall, Engine::Address::kAnimatedObjectsWait, kObjects);
		Installed(kObjects);
		installed = true;
	}

	Counters Take()
	{
		return { cellAnimations.Take(), animatedObjects.Take() };
	}

	void Hold()
	{
		if (installed)
			held.store(true, std::memory_order_release);
	}

	void Release()
	{
		if (held.exchange(false, std::memory_order_acq_rel))
			held.notify_all();
	}

	bool Installed() { return installed; }
}

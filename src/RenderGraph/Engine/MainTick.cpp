#include "RenderGraph/Engine/MainTick.h"

#include <mutex>
#include <string>
#include <vector>

namespace RenderGraph::Engine::MainTick
{
	namespace
	{
		struct Callback
		{
			std::string name;
			std::function<void()> run;
		};

		struct Ticks
		{
			// Recursive: a callback may run nested frames (a blocking movie) that reach the wait again.
			std::recursive_mutex mutex;
			std::vector<Callback> callbacks;
			bool installed = false;
			bool failed = false;
		};

		Ticks& Get()
		{
			static Ticks s_ticks;
			return s_ticks;
		}

		/** @brief JobListManager's frame-job wait (SE 34554, AE 35409), called only by Main::Update, on the main thread. */
		struct WaitForFrameJobs
		{
			static void thunk()
			{
				func();
				auto& ticks = Get();
				std::scoped_lock lock(ticks.mutex);
				for (const auto& callback : ticks.callbacks)
					callback.run();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool Add(std::string_view a_name, std::function<void()> a_callback)
	{
		auto& ticks = Get();
		std::scoped_lock lock(ticks.mutex);
		if (!ticks.installed && !ticks.failed) {
			stl::detour_thunk<WaitForFrameJobs>(REL::RelocationID(34554, 35409));
			ticks.installed = WaitForFrameJobs::func.address() != 0;
			ticks.failed = !ticks.installed;
			if (ticks.installed)
				logger::info("[Main tick] hooked the frame-job wait (Main::Update), the main thread's point after the frame's jobs");
		}
		if (!ticks.installed) {
			logger::error("[Main tick] the frame-job wait is not hooked; {} does not run", a_name);
			return false;
		}
		ticks.callbacks.push_back({ std::string(a_name), std::move(a_callback) });
		return true;
	}
}

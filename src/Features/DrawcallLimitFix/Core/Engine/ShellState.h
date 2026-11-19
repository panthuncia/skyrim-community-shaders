#pragma once

// Shell state that engine hooks outside the shell read: whether DCLF runs this frame, and where the engine's passes are.

namespace DCLF::ShellState
{
	namespace Detail
	{
		inline bool (*runningQuery)() = nullptr;
	}

	/** @brief Main thread, once at PostPostLoad, before the early hooks. Sets how Running asks the shell. */
	inline void InstallRunningQuery(bool (*a_running)()) { Detail::runningQuery = a_running; }
	/** @brief Any hook. Whether DCLF runs. False before the query is installed. */
	inline bool Running() { return Detail::runningQuery && Detail::runningQuery(); }

	/** @brief Where the engine's passes are. Render thread only, written by the shell's Main::RenderDepth thunk. */
	struct EnginePasses
	{
		bool inDepthPass = false;
		bool depthControlDriven = false;  // RenderDepth's second argument
	};
	/** @brief Render thread only. The engine's passes as last written. */
	inline EnginePasses& Passes()
	{
		static EnginePasses passes;
		return passes;
	}
}

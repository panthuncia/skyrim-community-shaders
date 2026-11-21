#pragma once

// Shell state that engine hooks outside the shell read: where the engine's passes are.
// DCLF's hooks go in only when it installs, and it stays on for the session once installed, so no hook asks whether it runs.

namespace DCLF::ShellState
{
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

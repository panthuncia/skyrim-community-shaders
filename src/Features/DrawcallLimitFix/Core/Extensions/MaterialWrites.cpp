// The frame VS variable's patch flag.

#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"

#include <atomic>

namespace DCLF::Extensions
{
	namespace
	{
		// Written on the main thread at install. Read by the material write hooks on the engine's update thread,
		// which are installed before the features, so it is atomic.
		std::atomic<bool> frameVSVariablePatched{ false };
	}

	bool FrameVSVariablePatched() { return frameVSVariablePatched.load(std::memory_order_relaxed); }

	void NoteFrameVSVariablePatch() { frameVSVariablePatched.store(true, std::memory_order_relaxed); }
}

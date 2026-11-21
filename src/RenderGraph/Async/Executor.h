#pragma once

#include <ORGModuleServices/Async/Executor.h>

namespace RenderGraph::Frame
{
	// CS's executor is OMS's, in its default layout: one coordinator thread, a preparation pool, one serial thread (Scene) and
	// the blocking threads (Streaming), plus the frame-critical threads. SharedExecutor (Lanes.h) makes CS's one instance.
	using Executor = org::async::Executor;
}

#pragma once

#include "Features/DrawcallLimitFix/Core/Runtime/SceneGraph.h"
#include "RenderGraph/Async/Lanes.h"

namespace DCLF
{
	/**
	 * @brief DCLF's scope and state graph on CS's shared executor (RenderGraph::Frame::SharedExecutor). See dclf-architecture.md, "The design".
	 * Frame-critical jobs jump the preparation queue and run at the coordinator's priority.
	 */
	class SceneScheduler
	{
	public:
		/** @brief The scope every feature job runs under. It is never cancelled. A job's own stop source cancels it. */
		static const org::async::Scope& Scope();
		static ::DCLF::Runtime::SceneGraph& Graph();
	};
}

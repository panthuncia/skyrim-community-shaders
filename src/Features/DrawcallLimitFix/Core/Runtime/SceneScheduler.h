#pragma once

#include "Features/DrawcallLimitFix/Core/Runtime/SceneExecutor.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneGraph.h"

#include <functional>
#include <string_view>

namespace org::async
{
	class SerializedTaskPump;
}

namespace DCLF
{
	/**
	 * @brief DCLF's threads, one executor for the whole feature. See dclf-architecture.md, "The design".
	 * The coordinator and the scene lane are one serialized thread each, above normal priority unless CS_DCLF_ASYNC_PRIORITY=normal.
	 * The preparation pool has hardware_concurrency - 2 threads, at least one, below normal priority. CS_DCLF_WORKERS sets the count.
	 * Frame-critical jobs jump the preparation queue and run at the coordinator's priority. Started on first use and never torn down.
	 */
	class SceneScheduler
	{
	public:
		static SceneExecutor& Executor();
		/**
		 * @brief The scene lane's scope: a serialized thread for the scene pump's passes only.
		 * The frame's builds keep the coordinator's thread, so a pass that spans the frame never holds one up.
		 */
		static const org::async::Scope& SceneLaneScope();
		/** @brief The scope every feature job runs under. It is never cancelled. A job's own stop source cancels it. */
		static const org::async::Scope& Scope();
		static unsigned PreparationWorkers();
		/** @brief The frame-critical threads. CS_DCLF_FRAME_WORKERS sets the count, by default a quarter of the pool and at least 2. */
		static unsigned FrameCriticalWorkers();
		static Runtime::SceneGraph& Graph();
		/**
		 * @brief A lane: a lock-free SerializedTaskPump whose wakes coalesce, running a_drain one pass at a time on a_executor.
		 * Never destroyed, because a completion may still wake it during process teardown.
		 */
		static org::async::SerializedTaskPump* MakeLanePump(SceneExecutor& a_executor, const org::async::Scope& a_scope, org::async::TaskClass a_class,
			org::async::TaskDispatch a_dispatch, std::string_view a_name, std::function<void()> a_drain, std::function<void()> a_refused);
	};
}

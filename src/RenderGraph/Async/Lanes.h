#pragma once

#include "RenderGraph/Async/Executor.h"

#include <ORGModuleServices/Async/Lanes.h>
#include <ORGModuleServices/Async/SerializedTaskPump.h>

#include <functional>
#include <memory>
#include <utility>

namespace org::runtime
{
	class ITaskService;
}

namespace RenderGraph::Frame
{
	/**
	 * @brief CS's one executor. The frame's lanes, the graph and program builds, and the features' jobs run on it.
	 * Made on first use and never destroyed, because joining threads in a DLL's static destruction is unreliable.
	 * The coordinator and serial threads run above normal priority, the preparation pool below normal, the blocking threads lowest.
	 * The preparation pool has hardware threads - 2, at least 1. The frame-critical threads are a quarter of the pool, at least 2.
	 */
	Executor& SharedExecutor();

	/**
	 * @brief ORG's task service (org::Runtime::Desc::tasks) over the shared executor, so ORG starts no threads of its own.
	 * OMS's org::async::CreateExecutorTaskService: frame-critical work goes to the frame-critical queue, everything else to the
	 * preparation pool, never to the blocking threads. A task that throws is fatal. Made on first use and never destroyed.
	 */
	std::shared_ptr<org::runtime::ITaskService> OrgTaskService();

	/** @brief The shared executor's preparation threads. */
	unsigned PreparationWorkers();

	/** @brief The shared executor's frame-critical threads. */
	unsigned FrameCriticalWorkers();

	/** @brief Any thread. Whether the calling thread is one of the shared executor's. */
	bool OnExecutorThread();

	/** @brief Any thread. Whether the calling thread is one of the shared executor's in a_class's domain. */
	bool OnExecutorThread(org::async::TaskClass a_class);

	/** @brief What a lane runs on. An empty refusal callback is fatal on CS's lanes. */
	using LaneDesc = org::async::LaneDesc;

	/** @brief a_desc, with an empty refusal callback replaced by a fatal one naming the lane. */
	LaneDesc FatalOnRefusal(LaneDesc a_desc);

	/**
	 * @brief A lane on the shared executor (org::async::Lane): a_drain runs one pass at a time. Wakes coalesce, and a wake during a pass
	 * runs one more. Stop drops its queued pass and waits for a running one. A refused task or a drain that throws is fatal unless
	 * the desc has its own refusal callback.
	 */
	class Lane : public org::async::Lane
	{
	public:
		Lane(LaneDesc a_desc, std::function<void()> a_drain) :
			org::async::Lane(SharedExecutor(), FatalOnRefusal(std::move(a_desc)), std::move(a_drain)) {}
	};

	/**
	 * @brief A lane that lives as long as the process, on the shared executor. Use it for work that a completion may still wake while
	 * the process tears down. Returns its pump.
	 */
	org::async::SerializedTaskPump& MakeLane(LaneDesc a_desc, std::function<void()> a_drain);

	/**
	 * @brief A lane with a queue on the shared executor (org::async::JobLane): each posted job runs once, in post order, one at a time.
	 * Stop drops the queued jobs and waits for a running one.
	 */
	template <class Job>
	class JobLane : public org::async::JobLane<Job>
	{
	public:
		JobLane(LaneDesc a_desc, std::function<void(Job&)> a_run) :
			org::async::JobLane<Job>(SharedExecutor(), FatalOnRefusal(std::move(a_desc)), std::move(a_run)) {}
	};
}

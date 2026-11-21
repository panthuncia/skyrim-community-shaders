#pragma once

#include "RenderGraph/Async/Executor.h"

#include <ORGModuleServices/Async/SerializedTaskPump.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
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
	 * Frame-critical work goes to the frame-critical queue, streaming work (graph builds) to the preparation pool, background
	 * work to the blocking threads. A task that throws is fatal. Made on first use and never destroyed.
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

	/** @brief What a lane runs on. */
	struct LaneDesc
	{
		std::string name;
		org::async::TaskClass taskClass = Executor::Preparation;
		org::async::TaskDispatch dispatch = org::async::TaskDispatch::Cpu;
		// Runs when the lane stops by itself: its task was refused, or its drain threw. Empty means fatal.
		std::function<void()> refused;
	};

	/**
	 * @brief A lane: a_drain runs one pass at a time on the shared executor. Wakes coalesce, and a wake during a pass runs one more.
	 * It has its own scope, so Stop drops its queued pass and waits for a running one.
	 */
	class Lane
	{
	public:
		Lane(LaneDesc a_desc, std::function<void()> a_drain);
		/** @brief Stops the lane. Never destroy it on the lane itself. */
		~Lane();
		Lane(const Lane&) = delete;
		Lane& operator=(const Lane&) = delete;

		/** @brief Any thread. Wakes the lane. False once it stopped. */
		bool Wake() { return pump.Notify(); }

		/** @brief The lane's pump, for callers that drain inline (TryRunInline) or read its state. */
		org::async::SerializedTaskPump& Pump() { return pump; }
		const org::async::SerializedTaskPump& Pump() const { return pump; }

		const std::string& Name() const { return desc.name; }

		/**
		 * @brief Teardown only, never on the lane itself. Stops the lane, drops a queued pass and waits for a running one.
		 * Later wakes return false.
		 */
		void Stop();

	private:
		LaneDesc desc;
		org::async::Scope scope;
		std::atomic<bool> stopped{ false };
		org::async::SerializedTaskPump pump;
	};

	/**
	 * @brief A lane that lives as long as the process. Use it for work that a completion may still wake while the process tears down.
	 * Returns its pump.
	 */
	org::async::SerializedTaskPump& MakeLane(LaneDesc a_desc, std::function<void()> a_drain);

	/**
	 * @brief A lane with a queue: each posted job runs once, in post order, one at a time.
	 * Stop drops the queued jobs and waits for a running one.
	 */
	template <class Job>
	class JobLane
	{
	public:
		JobLane(LaneDesc a_desc, std::function<void(Job&)> a_run) :
			run(std::move(a_run)), lane(std::move(a_desc), [this] { Drain(); }) {}
		~JobLane() { Stop(); }
		JobLane(const JobLane&) = delete;
		JobLane& operator=(const JobLane&) = delete;

		/** @brief Any thread. Queues a_job. False once stopped, and the job is dropped. */
		bool Post(Job a_job)
		{
			{
				const std::lock_guard lock(mutex);
				if (stopping)
					return false;
				queue.push_back(std::move(a_job));
			}
			return lane.Wake();
		}

		/** @brief Teardown only, never on the lane. Drops the queued jobs and waits for a running one. */
		void Stop()
		{
			std::deque<Job> dropped;
			{
				const std::lock_guard lock(mutex);
				stopping = true;
				dropped.swap(queue);
			}
			lane.Stop();
		}

		Lane& Underlying() { return lane; }

	private:
		void Drain()
		{
			for (;;) {
				std::optional<Job> job;
				{
					const std::lock_guard lock(mutex);
					if (stopping || queue.empty())
						return;
					job.emplace(std::move(queue.front()));
					queue.pop_front();
				}
				run(*job);
			}
		}

		std::mutex mutex;
		std::deque<Job> queue;
		bool stopping = false;
		std::function<void(Job&)> run;
		// Last, so it stops first and its running pass ends before the queue goes.
		Lane lane;
	};
}

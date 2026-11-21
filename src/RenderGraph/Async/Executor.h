#pragma once

#include <ORGModuleServices/Async/GraphScheduler.h>
#include <ORGModuleServices/Async/GraphSchedulingLayout.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace RenderGraph::Frame
{

	// CS's executor, shared by the frame's lanes and the features (SharedExecutor in Lanes.h): one coordinator thread, one serial
	// thread (Scene) and a configurable number of preparation threads.
	// Dispatch never waits and never runs work inline. A job goes into its domain's unbounded MPMC queue and a semaphore wakes a worker.
	// Scope waits and Shutdown are for teardown and tests, not normal frames.
	// Frame-critical jobs have their own queue, taken first, plus a few dedicated threads at frame-critical priority.
	// That way they still start when the engine's threads saturate the machine and the below-normal preparation workers are starved.
	class Executor final : public org::async::GraphScheduler
	{
	public:
		static constexpr org::async::TaskClass Coordinator{ 0, 0 };
		static constexpr org::async::TaskClass Preparation{ 0, 1 };
		static constexpr org::async::TaskClass Scene{ 0, 2 };
		// Blocking work, such as texture streaming's window reads and program builds, kept off the preparation pool the frame's work runs on.
		static constexpr org::async::TaskClass Streaming{ 0, 3 };
		// A frame-critical thread's class, used only in ThreadStart. Jobs reach these threads through DispatchFrameCritical.
		static constexpr org::async::TaskClass FrameCriticalThread{ 0, 4 };
		static constexpr org::async::GraphSchedulingLayout Layout{ 1, 4, Coordinator, Coordinator };
		static constexpr std::size_t kDomains = 4;
		static constexpr unsigned kStreamingWorkers = 2;

		struct Statistics
		{
			std::uint64_t accepted = 0, completed = 0, cancelled = 0, rejected = 0, failed = 0;
			std::array<std::size_t, kDomains> queued{}, active{}, highWater{};
		};

		/**
		 * @brief Cumulative statistics per ParallelFor name. A reporter takes deltas.
		 * blockedNanoseconds is time callers spent waiting on chunks a helper had claimed, which ParallelFor exists to keep small.
		 */
		struct ParallelForStatistics
		{
			std::string name;
			std::uint64_t calls = 0;                  // calls with a_count > 0
			std::uint64_t chunks = 0, helperChunks = 0;  // every chunk, and those a helper ran
			std::uint64_t blocked = 0;                // calls whose caller waited for a helper's chunk
			std::uint64_t blockedNanoseconds = 0, maxBlockedNanoseconds = 0;  // max since start, or since the last take
			std::uint64_t boosts = 0;                 // helpers raised to their caller's priority
			std::uint64_t idleHelpers = 0;            // helpers that started after every chunk was taken
		};

		// Called on each worker thread before it takes work, with its class and its index within the class.
		using ThreadStart = std::function<void(org::async::TaskClass, unsigned)>;
		/**
		 * @brief Get and set the calling thread's priority, using Windows THREAD_PRIORITY_* values.
		 * With both set, a helper running a ParallelFor caller's chunks runs at least at the caller's priority and is restored afterwards.
		 * Each worker reads its own once after ThreadStart. Nothing else may change a worker's priority.
		 */
		struct ThreadPriority
		{
			std::function<int()> get;
			std::function<void(int)> set;
			// A frame-critical job runs at least at this priority, restored after.
			int frameCritical = 0;
		};

		explicit Executor(unsigned preparationWorkers = 2, ThreadStart onThreadStart = {}, ThreadPriority priority = {}, unsigned frameCriticalWorkers = 0);
		~Executor() override;
		Executor(const Executor&) = delete;
		Executor& operator=(const Executor&) = delete;
		org::async::Scope CreateScope(std::string_view name) override;
		bool Dispatch(const org::async::Scope&, org::async::TaskClass, org::async::TaskDispatch,
			std::string_view, Task, org::async::TaskTraceMetadata = {}) override;
		bool DispatchAfter(const org::async::Scope&, std::chrono::steady_clock::duration,
			org::async::TaskClass, std::string_view, Task) override;
		/**
		 * @brief A preparation job the frame waits on, such as the CPU pipeline's culls. Queued ahead of every other preparation job
		 * and run at least at frame-critical priority. A ParallelFor inside it gets frame-critical helpers. Never runs inline.
		 */
		bool DispatchFrameCritical(const org::async::Scope& a_scope, std::string_view a_name, Task a_task);
		/**
		 * @brief Runs a_body(begin, end) over [0, a_count) in chunks of a_grain, on the caller and the preparation workers, and returns once all ran.
		 * The caller takes chunks itself and only waits for chunks a worker is running, never for queued work.
		 * Chunks are exactly [k * grain, min(count, (k + 1) * grain)), so callers may index results by a_begin / grain.
		 * A helper holding a chunk runs at least at the caller's priority. The first exception is rethrown once every claimed chunk finished.
		 * It waits, so never call it on the render thread.
		 */
		void ParallelFor(std::string_view a_name, std::size_t a_count, std::size_t a_grain, const std::function<void(std::size_t, std::size_t)>& a_body);
		Statistics GetStatistics() const;
		/** @brief Every ParallelFor name seen so far. a_takePeaks resets each maxBlockedNanoseconds once read. */
		std::vector<ParallelForStatistics> GetParallelForStatistics(bool a_takePeaks = false) const;
		// Owner must outlive its callbacks. Calling Shutdown or Wait on its own workers is an error.
		void Shutdown();

	private:
		struct State;
		struct ExecutorScope;
		struct Threads;
		std::shared_ptr<State> state;
		std::unique_ptr<Threads> threads;
		org::async::Scope parallelScope;  // ParallelFor's helpers
	};
}

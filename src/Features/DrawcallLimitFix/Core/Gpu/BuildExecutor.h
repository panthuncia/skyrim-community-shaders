#pragma once

// Executors for DCLF's pipeline work: a width-limited pump for compiles and builds, and the pipeline lane's serialized drain.
// Callable from any thread.

#include <functional>

namespace DCLF
{
	/**
	 * @brief The executor shader compiles and pipeline builds run on. Tasks queue without bound and run on the preparation pool,
	 * at most BuildWidth() at once, because a compile can block in DXC for seconds and the frame's jobs need the other workers.
	 * Nothing waits for these tasks. A stale result is ignored by its generation.
	 */
	std::function<bool(std::function<void()>)> BuildSubmitter();

	/** @brief How many builds run at once. Half the preparation workers, at least one. */
	unsigned BuildWidth();

	/**
	 * @brief Sets the pipeline lane's drain, run one pass at a time on the coordinator. Wakes coalesce, and a wake during a pass
	 * makes another pass. A wake before this is set runs an empty pass and its work waits for the next one.
	 */
	void SetPipelineLaneDrain(void (*a_drain)());
	void WakePipelineLane();
}

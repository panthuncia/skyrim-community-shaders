#pragma once

// Executors for DCLF's pipeline work: a width-limited pump for compiles and builds, and the pipeline lane's serialized drain.
// Callable from any thread.

#include <array>
#include <cstdint>
#include <functional>

namespace DCLF
{
	/** @brief What a build task is. Queued stages run before queued pipeline builds. */
	enum class BuildKind : std::uint8_t
	{
		Stage,     // a shader stage: a disk cache lookup, else a compile
		Pipeline,  // a pipeline build from ready stages
	};

	/**
	 * @brief The executor shader compiles and pipeline builds run on. Tasks queue without bound and run on the preparation pool,
	 * at most BuildWidth() at once, because a compile can block in DXC for seconds and the frame's jobs need the other workers.
	 * Nothing waits for these tasks. A stale result is ignored by its generation.
	 */
	std::function<bool(std::function<void()>)> BuildSubmitter(BuildKind a_kind = BuildKind::Pipeline);

	/** @brief CS_DCLF_STATS: the pump's tasks since the last call, by BuildKind. Times in ms. */
	struct BuildPumpStats
	{
		struct Spread
		{
			float p50 = 0, p95 = 0, max = 0, total = 0;
		};
		struct Kind
		{
			std::uint32_t tasks = 0;
			Spread wait;  // queued until a runner took it
			Spread run;
		};
		std::array<Kind, 2> kinds;
		std::uint32_t deepest = 0;  // the most tasks queued at once
		unsigned width = 0;
	};
	BuildPumpStats TakeBuildPumpStats();

	/** @brief How many builds run at once. Half the preparation workers, at least one. */
	unsigned BuildWidth();

	/**
	 * @brief Sets the pipeline lane's drain, run one pass at a time on the coordinator. Wakes coalesce, and a wake during a pass
	 * makes another pass. A wake before this is set runs an empty pass and its work waits for the next one.
	 */
	void SetPipelineLaneDrain(void (*a_drain)());
	void WakePipelineLane();
}

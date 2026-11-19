#pragma once

// The shadow payload's entry exclusions for both modes: the sun's over the sun candidates and the paraboloids' over the light
// candidates. Built on the snapshot builder, each with a cache kept up to date by what moved. Opaque to Core.

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowExclusionBuild.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/SunExclusion.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/EntrySpace.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Features::Shadows::Local
{
	struct SunExclusionIndex;  // ExclusionBuild.cpp

	/**
	 * @brief The last exclusion's inputs and verdicts, kept up to date by what moved.
	 * Judged whole only for a new cache or tables generation, or behind the change log's trim.
	 */
	struct SunExclusionCache
	{
		std::shared_ptr<const EntrySpace> candidates;
		LogCursor cursor;
		std::uint64_t membership = 0;
		std::uint64_t keptBuild = 0;  // the kept shadow build its inputs are as of (Api::ShadowExclusionBuild::keptBuild)
		std::vector<std::uint8_t> excluded;
		std::uint32_t excludedCount = 0;
		std::uint64_t version = 0;  // SunExclusion::version of excluded
		bool valid = false;
		std::shared_ptr<SunExclusionIndex> index;
		std::uint64_t builds = 0, reused = 0, translated = 0, same = 0;
		double buildMs = 0.0, buildMaxMs = 0.0;
	};

	/**
	 * @brief The next frame's entry exclusion of a_build's mode. A candidate stays in the mode's culls when one of its objects casts
	 * and is not an input, since the engine must still draw it. Null without candidates.
	 */
	std::shared_ptr<SunExclusion> BuildSunExclusion(const Api::ShadowExclusionBuild& a_build, SunExclusionCache* a_cache = nullptr);

	/** @brief Each mode's cache by its source. Snapshot builder thread, builds in frame order. */
	SunExclusionCache& ExclusionCacheOf(Api::ShadowSource a_source);

	/** @brief Builds a_build's exclusion with its source's cache. */
	void BuildExclusion(Api::ShadowExclusionBuild& a_build);
}

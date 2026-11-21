#pragma once

// Object LOD's segment events. Hooks on the engine's segment writers name each changed shape, and a coordinator
// component samples its drawn ranges again. Core owns the ranges. See dclf-architecture.md, "Object LOD: segments".

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace DCLF::Features::Lod
{
	/** @brief Scene lane. Counters since the last report. */
	struct SegmentState
	{
		struct Stats
		{
			std::uint64_t events = 0, changed = 0;
		} stats;
	};

	/** @brief Patches every segment writer's call, or none. */
	void InstallSegmentHooks();
	bool SegmentHooksInstalled();
	/** @brief Scene lane, the queue's only consumer. */
	std::size_t DrainSegmentEvents(Api::KeySink& a_sink);

	/** @brief Registers the channel, its component and its report line. */
	void RegisterSegmentChannel(Api::InstallContext& a_context);
}

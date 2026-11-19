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
		struct Parity
		{
			std::uint64_t checks = 0, shapes = 0, differ = 0;
			std::string first;
			std::uint32_t pass = 0;  // the check runs once a pass
		} parity;
	};

	/** @brief Patches every segment writer's call, or none. */
	void InstallSegmentHooks();
	bool SegmentHooksInstalled();
	/** @brief Scene lane, the queue's only consumer. */
	std::size_t DrainSegmentEvents(Api::KeySink& a_sink);

	/** @brief Registers the channel, its component and its report line. */
	void RegisterSegmentChannel(Api::InstallContext& a_context);

	/** @brief Notified on the scene lane after a pass applies its segment events. */
	struct SegmentsApplied
	{
		const Api::CoordinatorComponentContext& context;
	};

	/** @brief Checks the kept ranges against the live ones under CS_DCLF_PERSISTENT_PARITY. */
	void SubscribeSegmentParity();
}

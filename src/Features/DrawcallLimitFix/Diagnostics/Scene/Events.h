#pragma once

#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/MirrorEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/RootViewEvents.h"

#include <vector>

#include <cstdint>
#include <span>

namespace DCLF::Diagnostics
{
	/** @brief Scene points that carry no data. Coordinator thread unless marked render thread. */
	enum class ScenePoint : std::uint8_t
	{
		MirrorCheckPoint,        // at a pass's end (its events all drained)
		MirrorProbePoint,        // render thread, frame start, the live scene captured for the mirror's observers
		AccumulatePrepared,      // render thread, EarlyPrepass, the material captures served
	};

	// Scene hook points. Each runs on its Notify's thread, which is the coordinator's pass unless noted, and reads the
	// coordinator's state through SceneView. Mirror writes and probe points.

	/** @brief A hook's update was applied to the mirror. `key` is null when the mirror holds no record. */
	struct MirrorUpdateApplied
	{
		std::uint8_t type;
		const void* key;
		std::uint32_t fields;
	};

	/** @brief First in the coordinator's event drains. */
	struct EventsCollecting
	{
		Scene::Coordinator& store;
	};

	/** @brief A batch was applied to the mirror, with the objects named by its value-less events, by engine channel. */
	struct MirrorEventsApplied
	{
		Scene::Coordinator& store;
		std::span<const std::vector<const void*>> channelKeys;
	};
}

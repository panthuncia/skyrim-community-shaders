#pragma once

#include <Render/FrameArena.h>
#include <Render/RunRef.h>
#include <Render/RunSlots.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace RenderGraph
{
	/** @brief A client's staging region: an oversize region of the runtime's frame arena (org::FrameArena::AcquireOversize). */
	using Staging = std::shared_ptr<const org::FrameArena::Region>;

	/**
	 * @brief A staging region of at least a_bytes for a client's per-run CPU data, such as vertices and texture rows.
	 *
	 * It has a chunk of the frame arena to itself and starts at the chunk's first byte, so the chunk's view reads it at offset 0.
	 * It is not zeroed. It goes back to the arena when its last holder lets it go, which must be the recording that reads it
	 * (`a_recording.Retain(frame)`), so it comes back once the GPU is done with it. A steady frame reuses the freed chunks.
	 * Fatal, naming a_area, when it cannot be made. Any thread, in graph mode.
	 */
	Staging AcquireStaging(std::string_view a_area, std::uint64_t a_bytes);

	/** @brief Every live RunSlots, so a stage's commit can move its staging run's values onto the run it submits. Thread-safe. */
	using RunSlotsRegistry = org::RunSlotsRegistry;

	/** @brief One value per run, stored by a client's commit and taken by the pass that records that run. Thread-safe. */
	template <class T>
	using RunSlots = org::RunSlots<T>;
}


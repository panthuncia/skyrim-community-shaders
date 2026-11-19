#pragma once

// Diagnostic hook points for the LOD gates' observers.

#include <cstdint>
#include <span>
#include <utility>

namespace RE
{
	class BSGeometry;
}

namespace DCLF::Features::Lod
{
	/** @brief Scene lane, when a publication takes its deltas. */
	struct GateFlipsPublished
	{
	};

	/** @brief Render thread, at the frame's start. claims is opaque, read it through Diagnostics::SceneView::ClaimedPhases. */
	struct GateReleasing
	{
		const void* claims;
		std::uint64_t token;
		std::span<const std::pair<const RE::BSGeometry*, std::uint8_t>> incoming;  // with the flip's phases
		std::span<const RE::BSGeometry* const> outgoing;
		bool released;  // false when the gate was already forced or unknown
	};
}

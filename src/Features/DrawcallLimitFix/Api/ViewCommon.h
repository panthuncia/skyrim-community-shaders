#pragma once

// Shared by engine views (Api/Views.h) and shadow views (Api/ShadowViews.h): the engine points view work runs at, and a view's own bits.

#include <cstdint>

namespace DCLF::Api
{
	// Defined in Core/Extensions/Views.h.
	class ViewPointContext;

	/** @brief The engine points a view's work runs at while DCLF runs: the render half's on the render thread, FrameStartGame in the game half. */
	enum class ViewPoint : std::uint8_t
	{
		FrameStartBeforeCoverage,  // the render half's frame start, before coverage is decided
		FrameStartAfterTreeLod,    // the render half's frame start, after the draw families decide and before FrameValues::Kick
		DepthCommitted,            // inside the depth commit, before its epoch is submitted, or after the depth epoch if it did not commit
		FrameStartGame,            // the game half's frame start (engine reads; values go to the render half in a ticket part)
		Count,
	};

	/** @brief A view's set phase and GPU cull view bit. Neither may be Core's or another view's. */
	struct ViewBits
	{
		std::uint8_t phase = 0;
		std::uint32_t viewBit = 0;
	};
}

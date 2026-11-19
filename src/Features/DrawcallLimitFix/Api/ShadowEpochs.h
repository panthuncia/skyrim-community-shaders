#pragma once

// Handlers for the shadow epoch at the engine's shadow points. FrameDriver keeps the frame's bookkeeping and calls these where the work goes.

#include <cstdint>

namespace DCLF::Api
{
	/**
	 * @brief The shadow epochs. One contribution only. Render thread, while DCLF runs.
	 *
	 * - beforeShadowCasters: once per frame, before the engine selects its first shadow light.
	 * - occlusionReady: returns whether this frame's shadow epoch can draw occlusion map a_map.
	 * - afterShadowMaps: runs the shadow epoch with the maps taken in a_maps, and returns the maps drawn.
	 */
	struct ShadowEpochsContribution
	{
		void (*beforeShadowCasters)() = nullptr;
		void (*beforeShadowMaps)() = nullptr;
		std::uint32_t (*afterShadowMaps)(std::uint32_t a_maps) = nullptr;
		bool (*occlusionReady)(std::uint32_t a_map) = nullptr;
	};
}

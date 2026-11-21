#pragma once

#include <cstdint>

// The segments alone, for headers that need their count. Keep the often-changing description fields out of this header.

namespace RenderGraph::Frame
{
	/**
	 * @brief The declared frame's segments, in frame order. PreDepth holds the EarlyPrepass clients. PreMain holds the depth
	 * copy, Terrain Blending, the shadow mask, light culling and the Prepass clients.
	 */
	enum class SegmentId : std::uint8_t
	{
		Start,           // reflections prepass and reflection faces' sky, up to DCLF's shadow view
		PreDepth,
		PreMain,
		Deferred,        // deferred passes, composite, water mask, RenderWorld scopes, kMAIN copy and water
		Water,           // after DCLF's water mask: the engine's water mask, RenderWorld scopes, kMAIN copy and water
		PostWater,       // after DCLF's water surface: the engine's water, its blend and the post-water composite
		Sun,             // the sun's occlusion query, after DCLF's forward epoch and before the first-person one
		SunWater,        // under water, after DCLF's water surface: the engine's water, its blend and the sun's query
		Post,            // volumetric lighting through the tonemap, up to RenderEndOfFrameEffects' return
		Ui,              // every frame kind, the menus, movies and overlay up to just before the present
		Count
	};
	inline constexpr std::uint32_t kSegments = static_cast<std::uint32_t>(SegmentId::Count);

	const char* SegmentName(SegmentId a_segment);
}

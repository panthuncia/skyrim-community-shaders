#pragma once

#include <cstdint>

// The declared frame's segments alone, for headers that need their count but not the description
// (RenderGraphRuntime.h). The description's fields change often and only a few files read them:
// keep them in FrameDescription.h, and keep this header free of them.

namespace RenderGraph::Frame
{
	/**
	 * @brief The declared frame's segments, in frame order: the start stage, the plan's S1 (pre-depth: the EarlyPrepass clients), S2 (pre-main: the
	 * depth copy at Main::RenderDepth's end, Terrain Blending, the shadow mask, light culling and the Prepass clients) and S3
	 * (deferred: the deferred passes and the composite). The others are added by later stages.
	 */
	enum class SegmentId : std::uint8_t
	{
		Start,           // the frame's start: the reflections prepass, the reflection faces' sky (the faces cleared by its load operations), to DCLF's shadow view
		PreDepth,
		PreMain,
		Deferred,        // deferred+water: the deferred passes and the composite, the water mask, the RenderWorld scopes, the kMAIN copy, the water and its blend
		Sun,             // the sun stage: the sun's occlusion query after DCLF's forward epoch, before the first-person one
		Post,            // the post stage: the volumetric lighting, the pre-tonemap composites, the upscaler's evaluate and the depth upscale,
		                 // the end-of-frame effects with the tonemap, to RenderEndOfFrameEffects' return
		Ui,              // the UI stage, in every frame kind: the frozen composite, the displays before and after a menu's 3D scene, the
		                 // scene and its composite, the movie's tiles, the overlay and the capture, to just before the present
		Count
	};
	inline constexpr std::uint32_t kSegments = static_cast<std::uint32_t>(SegmentId::Count);

	/** @brief "pre-depth". */
	const char* SegmentName(SegmentId a_segment);
}

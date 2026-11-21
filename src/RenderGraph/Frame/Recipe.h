#pragma once

#include "RenderGraph/Frame/FrameDescription.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace org
{
	struct ScheduleTrace;
	class GraphHost;
	class RenderGraph;
}

/**
* @brief The recipe: CS's frame as a fixed, ordered list of stages, one recipe per frame kind. The table is in Frame/RecipeTable.h.
*
* Recipe order is pass order. A check proves every resource a pass reads is written earlier in the recipe, or comes from the
* previous frame or from outside the graphs. A CS stage is one revision-driven epoch with one ticket a frame.
* A pass's order in its stage is its step, then its order key. The check reads each epoch's schedule as its graph was built,
* so what is printed and checked is what the frames run.
* The header stays free of ORG includes.
*/
namespace RenderGraph::Frame::Recipes
{
	/** @brief The world recipe's stages, in frame order. */
	enum class StageId : std::uint8_t
	{
		Start,        // CS: the decal tests, the face tint, the reflection faces' sky, the water effects, the shadow clears DCLF does not host
		Shadows,      // Drawcall Limit Fix's shadow views and Skylighting's occlusion
		PreDepth,     // CS: the EarlyPrepass clients, the main depth's clear DCLF's Z-prepass does not host
		Depth,        // Drawcall Limit Fix's Z-prepass and first-person depth
		PreMain,      // CS: the depth copy, Terrain Blending, the shadow mask, light culling, the Prepass clients and the main pass's sky
		Opaque,       // Drawcall Limit Fix's main opaque pass
		Deferred,     // CS: deferred+water: the deferred passes and the composite, the water mask, the RenderWorld scopes, the water and its blend
		WaterMask,    // Drawcall Limit Fix's water mask
		Water,        // CS: water, the deferred+water stage's steps after DCLF's water mask
		WaterSurface, // Drawcall Limit Fix's water surface. Under water it comes after the sun stage's first steps
		PostWater,    // CS: post-water, the water stage's steps after DCLF's water surface
		Forward,      // Drawcall Limit Fix's forward pass
		Sun,          // CS: the precipitation, under water the water, the sun's occlusion query (the first person's query is the post stage's)
		SunWater,     // CS: sun water, under water the sun stage's steps after DCLF's water surface
		FirstPerson,  // Drawcall Limit Fix's first-person colour
		Post,         // CS: the post chain, the upscaler's evaluate and the depth upscale, one stage
		Ui, // CS: the UI stage, in every frame kind. HDR output keeps its own ticket
		Count
	};
	// The table (RecipeTable.h) lists the stages in this order.
	inline constexpr std::uint32_t kStages = static_cast<std::uint32_t>(StageId::Count);

	/** @brief Who owns a stage's work. */
	enum class Owner : std::uint8_t
	{
		Cs,    // a CS stage: one revision-driven epoch, recorded per description and frame slot, one ticket
		Fragment,  // a fragment source's own graphs (Drawcall Limit Fix's), recorded off the frame and adopted as its fragment
	};

	/** @brief "pre-main". */
	const char* StageName(StageId a_stage);

	/** @brief One epoch of the world recipe at its stage's place in the frame. */
	struct PositionedEpoch
	{
		std::uint32_t epoch = 0;
		StageId stage = StageId::Count;
		Owner owner = Owner::Cs;
	};
	/** @brief Every epoch of the world recipe's stages with its stage. HDR output and frame generation are left out. Any thread, after init. */
	std::vector<PositionedEpoch> PositionedEpochs();

	/** @brief Whether kind a_kind's recipe has CS stage a_segment. a_capture means a frozen world's capture frame. */
	bool KindHas(Kind a_kind, bool a_capture, SegmentId a_segment);
	/** @brief The kind's recipe as one line: "menu scene (tickets) | UI". */
	std::string DescribeKind(Kind a_kind, bool a_capture);
	/** @brief What changes between two kinds' recipes (empty when it is the same recipe), for the frame-kind switch's log line. */
	std::string DiffKinds(Kind a_from, bool a_fromCapture, Kind a_to, bool a_toCapture);

	/** @brief An epoch graph's schedule as it was built: its split for a_epoch, or its whole executable. Null before one is selected. */
	std::shared_ptr<const org::ScheduleTrace> TraceEpochGraph(org::RenderGraph& a_graph, std::uint32_t a_epoch);

	/** @brief Whether the recipe check reads epoch a_epoch's schedule. Any thread, after init. */
	bool Traced(std::uint32_t a_epoch);

	/** @brief On the build thread, checks the recipe with a new build in place before it attaches. A bad read order stops development builds early.
	*  Only once a whole check has seen every epoch's build. */
	void Accept(std::uint32_t a_epoch, const std::shared_ptr<const org::ScheduleTrace>& a_trace);

	/** @brief Records an epoch's schedule as a build made it. The build manager calls it at attach. Any thread. */
	void NoteTrace(std::uint32_t a_epoch, std::uint64_t a_generation, std::shared_ptr<const org::ScheduleTrace> a_trace);

	/** @brief At Renderer::Begin, rechecks and prints the recipe when an epoch runs a build not seen yet.
	*  A read whose only producers come later in the frame is a mismatch. */
	void Check(org::GraphHost& a_host);
}

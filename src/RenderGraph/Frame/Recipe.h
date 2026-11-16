#pragma once

#include "RenderGraph/Frame/FrameDescription.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

namespace org
{
	struct ScheduleTrace;
	class GraphHost;
	class RenderGraph;
}

/**
 * @brief The recipe: CS's frame as a fixed, ordered list of stages, one recipe per frame kind (docs/development/org-native-frame-graph.md,
 * "The recipe"). The shape follows BasicRenderer's PipelineRecipe: recipe order is pass order, and a check proves that every resource a
 * pass reads is written earlier in the recipe, or comes from the previous frame or from outside the graphs.
 *
 * - A **stage** is one or more graph epochs at a fixed place in the frame. A CS stage is one revision-driven epoch (a FrameAnchors
 *   declared segment): one graph built off the render thread, one kept recording per frame slot, one ticket a frame, submitted at its
 *   commit point. Drawcall Limit Fix's stages are its own graphs. A stage not moved onto the recipe yet still runs its anchors' tickets
 *   at their hooks; the recipe lists those epochs in their place so the check sees what they read and write.
 * - A **pass** is contributed by a feature (a FrameAnchors client's PassDesc) to the stage that holds its anchor. Its order in the
 *   stage is the stage's graph order: the step, then the order key.
 * - The check reads each epoch's schedule as its graph was built (org::RenderGraph::TraceSelectedSchedule, traced on the build thread
 *   when the build manager builds it, else once on the render owner when the frame starts), so the recipe printed and checked is the
 *   one the frames run.
 *
 * Header deliberately free of ORG includes.
 */
namespace RenderGraph::Frame::Recipes
{
	/** @brief The world recipe's stages, in frame order. */
	enum class StageId : std::uint8_t
	{
		Start,        // CS: the reflections prepass and the reflection faces' sky (the decal tests before it keep their tickets)
		Shadows,      // Drawcall Limit Fix's shadow views and Skylighting's occlusion
		PreDepth,     // CS: the EarlyPrepass clients
		Depth,        // Drawcall Limit Fix's Z-prepass and first-person depth
		PreMain,      // CS: the depth copy, Terrain Blending, the shadow mask, light culling, the Prepass clients and the main pass's sky
		Opaque,       // Drawcall Limit Fix's main opaque pass
		Deferred,     // CS: deferred+water: the deferred passes and the composite, the water mask, the RenderWorld scopes, the water and its blend
		Forward,      // Drawcall Limit Fix's forward pass, with the frame-group clears' ticket
		Sun,          // CS: the sun's occlusion query after the forward pass (the first person's query is the post stage's)
		FirstPerson,  // Drawcall Limit Fix's first-person colour
		Post,         // CS: the post chain, the upscaler's evaluate and the depth upscale, one stage
		Ui,           // CS: the UI stage (a Bink frame's depth clear, Scaleform's displays, the movie tiles, the overlay); HDR output and the capture
		              // keep their tickets
		Count
	};
	inline constexpr std::uint32_t kStages = static_cast<std::uint32_t>(StageId::Count);

	/** @brief Who owns a stage's work. */
	enum class Owner : std::uint8_t
	{
		Cs,       // a CS stage: one revision-driven epoch, recorded per description and frame slot, one ticket
		Dclf,     // Drawcall Limit Fix's own graphs
		Tickets,  // anchors' own epochs, a ticket at each hook (not moved onto the recipe yet)
	};

	/** @brief "pre-main". */
	const char* StageName(StageId a_stage);

	/**
	 * @brief The recipes of the other frame kinds (docs/development/org-native-frame-graph.md, "Frame kinds and the UI stage"). Each kind
	 * has its own ordered list of stages, all built ahead: the world (and a frozen world's capture frame), a menu over a frozen world,
	 * the main menu, a loading screen, a Bink movie. A CS stage a kind does not list is absent from its frames.
	 * Whether kind a_kind's recipe (a_capture: a frozen world's capture frame) has CS stage a_segment.
	 */
	bool KindHas(Kind a_kind, bool a_capture, SegmentId a_segment);
	/** @brief The kind's recipe as one line: "menu scene (tickets) | UI". */
	std::string DescribeKind(Kind a_kind, bool a_capture);
	/** @brief What changes between two kinds' recipes (empty when it is the same recipe), for the frame-kind switch's log line. */
	std::string DiffKinds(Kind a_from, bool a_fromCapture, Kind a_to, bool a_toCapture);

	/** @brief An epoch graph's schedule as it was built: its split for a_epoch, or its whole executable. Null before one is selected. */
	std::shared_ptr<const org::ScheduleTrace> TraceEpochGraph(org::RenderGraph& a_graph, std::uint32_t a_epoch);

	/**
	 * @brief Whether the recipe check reads epoch a_epoch's schedule: an epoch of a world-recipe stage. Any thread, after the render graph
	 * initialized.
	 */
	bool Traced(std::uint32_t a_epoch);

	/**
	 * @brief An epoch's schedule as a build made it (a_generation: the build generation the epoch runs once the build is attached).
	 * The build manager calls it when it attaches the build. Any thread.
	 */
	void NoteTrace(std::uint32_t a_epoch, std::uint64_t a_generation, std::shared_ptr<const org::ScheduleTrace> a_trace);

	/**
	 * @brief At Renderer::Begin, on the render owner (the frame driver, before the stages' schedules): when an epoch of the world recipe
	 * runs a build the recipe has not seen, its schedule is read, the recipe is checked again, and the recipe is printed when its passes
	 * changed. A finding (a read whose only producers come later in the frame, and that is not declared as the previous frame's) is a
	 * mismatch: fatal in development builds, logged once in release builds (Frame::Driver::MismatchFatal).
	 */
	void Check(org::GraphHost& a_host);

	/** @brief The census line for CS_ORG_EPOCH_STATS: the recipe's passes, its inputs by kind, and its findings. */
	void LogStats();
}

#pragma once

#include "RenderGraph/Frame/FrameDescription.h"
#include "RenderGraph/Frame/Recipe.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string_view>

/**
* @brief The recipe table: CS's whole frame in one constexpr table. Everything else is derived from it.
*
* Frame kinds name their stages in order. A stage is a CS stage, with one graph and one ticket, or a Drawcall Limit Fix graph.
* A CS stage is an ordered list of steps. A step is one engine hook: an anchor with the client ids it holds, another module's
* epoch, or a point that only ends the stage. A whole step holds its anchor: no other stage has a step there. Anchors have no
* epochs of their own.
* Membership is explicit. A client may register only where a step names it, or it is a recipe mismatch.
* The header stays free of ORG includes.
*/
namespace RenderGraph::Frame::Recipes::Table
{
	using Anchor = FrameAnchors::Anchor;
	using Segment = RenderGraphRuntime::Segment;

	// Prepass holds eleven with Physical Sky's cloud clients (cs.physical-sky.cloud-maps and cs.physical-sky.clouds) and Light Limit Fix's culling.
	inline constexpr std::size_t kMaxNames = 11;
	inline constexpr std::size_t kMaxSteps = 21;
	inline constexpr std::size_t kMaxEpochs = 10;
	inline constexpr std::size_t kMaxCommitBefore = 20;

	/** @brief Client ids, in no particular order; the unused tail is empty. */
	using Names = std::array<std::string_view, kMaxNames>;

	constexpr bool Has(const Names& a_names, std::string_view a_id)
	{
		for (const auto name : a_names)
			if (!name.empty() && name == a_id)
				return true;
		return false;
	}

	/** @brief One step of a CS stage. */
	struct Step
	{
		Anchor anchor = Anchor::Count;     // Count for a point step
		Names clients{};                   // the clients the step holds at its anchor
		bool whole = false;                // the stage holds the anchor wholly: no other stage has a step at it
		bool point = false; // a CS point that is no anchor. It holds no passes and ends the stage
		// Takes taps only until a later step of its stage has tapped this frame. Then its anchor's taps go to the next step there.
		bool untilLater = false;
		// The instance range this step makes and taps, from firstInstance. A count of 0 means every instance from there.
		// Two steps at one anchor give the same client disjoint ranges.
		std::uint8_t firstInstance = 0;
		std::uint8_t instanceCount = 0;

		constexpr bool Empty() const { return anchor == Anchor::Count && !point; }
	};

	/** @brief An epoch in a stage's part of the frame: a CS stage's own graph, or one of Drawcall Limit Fix's. */
	struct Epoch
	{
		Segment segment = Segment::Count;
		// A fragment source's stage epoch it must cover whenever it draws (cleanup plan A3, step 6). The source names each frame the
		// engine draws it instead, by an engine state set at the adoption point. Any other uncovered frame is TapError::Uncovered.
		bool covered = false;
	};

	/** @brief A list of epochs; the unused tail is Segment::Count. */
	using SegmentList = std::array<Segment, kMaxCommitBefore>;

	constexpr SegmentList Segments(std::initializer_list<Segment> a_segments)
	{
		SegmentList out{};
		for (auto& segment : out)
			segment = Segment::Count;
		std::size_t i = 0;
		for (const auto segment : a_segments)
			out[i++] = segment;
		return out;
	}

	/** @brief One stage of the frame. */
	struct Stage
	{
		StageId id = StageId::Count;
		Owner owner = Owner::Cs;
		std::string_view name;  // "pre-main"
		// A CS stage's epoch (RenderGraphRuntime::StageSegment) and its names; Count for Drawcall Limit Fix's stages.
		SegmentId segment = SegmentId::Count;
		FrameAnchors::SegmentNames names{};
		std::array<Step, kMaxSteps> steps{};
		// The window opens only once every earlier stage of the frame has ended. Otherwise a point-ended stage that staged nothing does not hold it back.
		bool opensLast = false;
		// A new description switches every frame slot at once, when all are ready. For the UI stage, whose description follows the frame kind.
		bool switchesEverySlot = false;
		// A point-ended stage commits just before any of these epochs is submitted (FrameAnchors::BeforeGpuWork), if it has not ended yet.
		SegmentList commitBefore = Segments({});
		// The same, but only while the stage holds staged work. A stage that staged nothing stays open, so its later steps take what
		// follows. For an epoch that comes before the stage in some frames and inside it in others.
		SegmentList commitBeforeIfStaged = Segments({});
		std::string_view commit;      // where it commits, for the recipe print
		std::string_view commitSite;  // the engine call that commits it
		std::array<Epoch, kMaxEpochs> epochs{};
	};

	constexpr Segment StageSegment(SegmentId a_segment) { return RenderGraphRuntime::StageSegment(static_cast<std::uint32_t>(a_segment)); }

	constexpr Epoch Own(Segment a_segment) { return { a_segment }; }
	/** @brief A fragment source's own epoch that it covers whenever it draws. */
	constexpr Epoch Covered(Segment a_segment) { return { a_segment, true }; }

	// Epochs whose submission commits the point-ended world stages when their usual next GPU work does not come.
	inline constexpr Segment kEndsWorld[] = { Segment::Forward, Segment::FirstPerson, StageSegment(SegmentId::Post) };
	// The epochs around the water: DCLF's water mask, the water stage, DCLF's water surface and the post-water stage.
	inline constexpr Segment kWaterEpochs[] = { Segment::WaterMask, StageSegment(SegmentId::Water), Segment::WaterSurface, StageSegment(SegmentId::PostWater) };

	/** @brief The world's stages in frame order. Recipe order is pass order. Each CS stage's comment names its commit point. */
	inline constexpr std::array<Stage, kStages> kStageTable{ {
		// The frame's start: decal tests, the face tint at a load, the cubemap faces, water effects and the shadow map clears DCLF does not host.
		// Commits just before the next GPU work, DCLF's shadow view or a later stage, at the latest when Main::RenderShadowMaps returns.
		{ .id = StageId::Start,
			.owner = Owner::Cs,
			.name = "start",
			.segment = SegmentId::Start,
			.names = { "CS stage: start", "CS stage: start inputs", "FrameSegments::Start / " },
			.steps = { {
				{ .anchor = Anchor::DecalQueries, .clients = { "cs.decals.occlusion" }, .whole = true },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.facetint" } },
				{ .anchor = Anchor::ReflectionsPrepass, .clients = { "cs.cloud-shadows", "cs.cloud-shadows.layers", "cs.view-atmosphere.reflection" }, .whole = true },
				{ .anchor = Anchor::SkyReflections, .clients = { "cs.sky.faces", "cs.cloud-shadows.layers" }, .whole = true },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.start" } },
				{ .anchor = Anchor::EngineShadowClear, .clients = { "cs.engine.skeleton.shadow@start" } },
				{ .point = true },
			} },
			.commitBefore = Segments({ Segment::ShadowView, Segment::SkyOcclusion, Segment::ZPrepass, Segment::FirstPersonDepth, Segment::MainOpaque,
				StageSegment(SegmentId::PreDepth), StageSegment(SegmentId::PreMain), StageSegment(SegmentId::Deferred), kWaterEpochs[0], kWaterEpochs[1], kWaterEpochs[2],
				kWaterEpochs[3], kEndsWorld[0], kEndsWorld[1], kEndsWorld[2] }),
			.commit = "just before DCLF's shadow view epoch, or Main::RenderShadowMaps' return",
			.commitSite = "BeforeGpuWork, before DCLF's shadow view epoch or later GPU work; at the latest the EarlyPrepass hook's start (Deferred::EarlyPrepasses)",
			.epochs = { Own(StageSegment(SegmentId::Start)) } },
		{ .id = StageId::Shadows,
			.owner = Owner::Fragment,
			.name = "shadows",
			.commit = "Drawcall Limit Fix's shadow views",
			.epochs = { Covered(Segment::ShadowView), Covered(Segment::SkyOcclusion) } },
		// Pre-depth: the EarlyPrepass clients, then the main depth clear when DCLF's Z-prepass does not host it.
		// Commits before DCLF's Z-prepass or a later stage, since the clear must reach the GPU before the Z-prepass draws.
		{ .id = StageId::PreDepth,
			.owner = Owner::Cs,
			.name = "pre-depth",
			.segment = SegmentId::PreDepth,
			.names = { "CS segment: pre-depth", "CS segment: pre-depth inputs", "FrameSegments::PreDepth / " },
			.steps = { {
				{ .anchor = Anchor::EarlyPrepass,
					.clients = { "cs.cloud-shadows.layers", "cs.grass-collision", "cs.llf.local-shadows", "cs.physical-sky.luts", "cs.terrain-blending.terrain-depth-clear",
						"cs.terrain-shadows", "cs.true-pbr.glints", "cs.volumetric-shadows" },
					.whole = true },
				{ .anchor = Anchor::EngineFrameClear, .clients = { "cs.engine.skeleton.frame@pre-depth" } },
				{ .point = true },
			} },
			.commitBefore = Segments({ Segment::ZPrepass, Segment::FirstPersonDepth, Segment::Reflection, Segment::MainOpaque, StageSegment(SegmentId::PreMain),
				StageSegment(SegmentId::Deferred), kWaterEpochs[0], kWaterEpochs[1], kWaterEpochs[2], kWaterEpochs[3], kEndsWorld[0], kEndsWorld[1], kEndsWorld[2] }),
			.commit = "just before DCLF's Z-prepass, or Main::RenderDepth's depth copy",
			.commitSite = "BeforeGpuWork, before DCLF's Z-prepass or later GPU work; at the latest Main::RenderDepth's depth copy (Engine/Skeleton, RunCopy)",
			.epochs = { Own(StageSegment(SegmentId::PreDepth)) } },
		// Drawcall Limit Fix's water reflection faces come after the Z-prepass (TESWaterReflections::Update).
		{ .id = StageId::Depth,
			.owner = Owner::Fragment,
			.name = "depth",
			.commit = "Main::RenderDepth",
			.epochs = { Covered(Segment::ZPrepass), Covered(Segment::FirstPersonDepth), Covered(Segment::Reflection) } },
		// Pre-main: the depth copy, the Prepass clients (Light Limit Fix's culling first) and the main pass's sky batch. Nothing else runs in between.
		// The sky batch does not run every frame, so it ends at a point just before DCLF's main opaque epoch or a later stage.
		{ .id = StageId::PreMain,
			.owner = Owner::Cs,
			.name = "pre-main",
			.segment = SegmentId::PreMain,
			.names = { "CS segment: pre-main", "CS segment: pre-main inputs", "FrameSegments::PreMain / " },
			.steps = { {
				{ .anchor = Anchor::EngineDepthCopy, .clients = { "cs.engine.skeleton.depth-copy" } },
				{ .anchor = Anchor::DepthDone, .clients = { "cs.terrain-blending" }, .whole = true },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.engine.skeleton.frame@engine-draws", "cs.shadow-mask.world" } },
				{ .anchor = Anchor::Prepass,
					.clients = { "cs.deferred.clears", "cs.engine.skeleton.frame@prepass", "cs.exp-height-fog", "cs.ibl", "cs.llf.light-culling", "cs.physical-sky",
						"cs.physical-sky.cloud-maps", "cs.physical-sky.clouds", "cs.screen-space-shadows", "cs.skylighting", "cs.view-atmosphere" },
					.whole = true },
				{ .anchor = Anchor::Sky, .clients = { "cs.sky.main" }, .whole = true },
				{ .point = true },
			} },
			.commitBefore = Segments({ Segment::MainOpaque, StageSegment(SegmentId::Deferred), kWaterEpochs[0], kWaterEpochs[1], kWaterEpochs[2], kWaterEpochs[3],
				kEndsWorld[0], kEndsWorld[1], kEndsWorld[2] }),
			.commit = "just before DCLF's main opaque epoch (the main pass's sky)",
			.commitSite = "BeforeGpuWork, before DCLF's main opaque epoch or later GPU work; at the latest CS's depth copy in Deferred::DeferredPasses (Engine/Skeleton, RunCopy)",
			.epochs = { Own(StageSegment(SegmentId::PreMain)) } },
		{ .id = StageId::Opaque, .owner = Owner::Fragment, .name = "opaque", .commit = "the main pass's opaque batches", .epochs = { Covered(Segment::MainOpaque) } },
		// Deferred+water: the deferred passes, composite, water mask, reflections, SSAO, kMAIN copy and water, after DCLF's main opaque.
		// Commits just before DCLF's water mask epoch, else just before DCLF's forward epoch. The water steps are gated and do nothing
		// when no water is in view. It keeps every step to the water's end, for the frames DCLF submits no water epoch.
		{ .id = StageId::Deferred,
			.owner = Owner::Cs,
			.name = "deferred+water",
			.segment = SegmentId::Deferred,
			.names = { "CS stage: deferred+water", "CS stage: deferred+water inputs", "FrameSegments::Deferred / " },
			.steps = { {
				{ .anchor = Anchor::DeferredPasses,
					.clients = { "cs.dyncubemaps", "cs.engine.skeleton.depth-copy@deferred-passes", "cs.ssgi", "cs.subsurface" },
					.whole = true },
				// Physical Sky's aerial perspective right after the composite, which is built without it (ORGPhysicalSky).
				{ .anchor = Anchor::DeferredComposite, .clients = { "cs.deferred", "cs.physical-sky.composite" }, .whole = true },
				{ .anchor = Anchor::PostComposite, .clients = { "cs.e11-rays" }, .whole = true },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.engine.skeleton.frame@engine-draws", "cs.water.mask" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.world.scopes" } },
				{ .anchor = Anchor::EngineCopy, .clients = { "cs.engine.skeleton.main-copy" } },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.water.surface" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.skeleton.frame@image-space", "cs.engine.imagespace.world.water" } },
				// The post-water composite of the medium into kMAIN, after the water is blended in (ViewAtmosphere's post-water hook).
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.view-atmosphere.composite" } },
				{ .point = true },
			} },
			.commitBefore = Segments({ kWaterEpochs[0], kWaterEpochs[2], kEndsWorld[0], kEndsWorld[1], kEndsWorld[2] }),
			.commit = "just before DCLF's water mask epoch, else the engine's blended draw (just before DCLF's forward epoch)",
			.commitSite = "BeforeGpuWork before DCLF's water mask epoch, else the engine's blended draw for the main accumulator (Features/Forward's ForwardHooks.cpp)",
			.epochs = { Own(StageSegment(SegmentId::Deferred)) } },
		// DCLF's water mask, at the engine's mask batch, in frames where DCLF has water to draw.
		{ .id = StageId::WaterMask, .owner = Owner::Fragment, .name = "water mask", .commit = "the engine's water mask batch", .epochs = { Covered(Segment::WaterMask) } },
		// Water: the deferred+water stage's steps after the mask, once DCLF's mask epoch ended that stage. The engine's mask of the water
		// DCLF does not claim comes first. Commits just before DCLF's water surface epoch, else at the blended draw.
		{ .id = StageId::Water,
			.owner = Owner::Cs,
			.name = "water",
			.segment = SegmentId::Water,
			.names = { "CS stage: water", "CS stage: water inputs", "FrameSegments::Water / " },
			.steps = { {
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.engine.skeleton.frame@engine-draws", "cs.water.mask" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.world.scopes" } },
				{ .anchor = Anchor::EngineCopy, .clients = { "cs.engine.skeleton.main-copy" } },
				// Geometry group 10's water, which the engine draws just before the water batch, then the rest if no surface epoch comes.
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.water.surface" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.skeleton.frame@image-space", "cs.engine.imagespace.world.water" } },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.view-atmosphere.composite" } },
				{ .point = true },
			} },
			.commitBefore = Segments({ kWaterEpochs[2], kEndsWorld[0], kEndsWorld[1], kEndsWorld[2] }),
			.commit = "just before DCLF's water surface epoch, else the engine's blended draw",
			.commitSite = "BeforeGpuWork before DCLF's water surface epoch, else the engine's blended draw (ForwardHooks.cpp)",
			.epochs = { Own(StageSegment(SegmentId::Water)) } },
		// DCLF's water surface, at the engine's water batch. Under water that batch comes after the blended draw, inside the sun stage.
		{ .id = StageId::WaterSurface, .owner = Owner::Fragment, .name = "water surface", .commit = "the engine's water batch", .epochs = { Covered(Segment::WaterSurface) } },
		// Post-water: the steps after the water surface, once DCLF's surface epoch ended the stage before. The engine's water of the water
		// DCLF does not claim comes first. Commits at the blended draw.
		{ .id = StageId::PostWater,
			.owner = Owner::Cs,
			.name = "post-water",
			.segment = SegmentId::PostWater,
			.names = { "CS stage: post-water", "CS stage: post-water inputs", "FrameSegments::PostWater / " },
			.steps = { {
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.water.surface" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.skeleton.frame@image-space", "cs.engine.imagespace.world.water" } },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.view-atmosphere.composite" } },
				{ .point = true },
			} },
			.commitBefore = Segments({ kEndsWorld[0], kEndsWorld[1], kEndsWorld[2] }),
			.commit = "the engine's blended draw (just before DCLF's forward epoch)",
			.commitSite = "the engine's blended draw for the main accumulator (ForwardHooks.cpp), else BeforeGpuWork",
			.epochs = { Own(StageSegment(SegmentId::PostWater)) } },
		{ .id = StageId::Forward, .owner = Owner::Fragment, .name = "forward", .commit = "the engine's blended draw", .epochs = { Covered(Segment::Forward) } },
		// Sun: precipitation draws, then the sun occlusion query, which must reach the GPU before first-person geometry occludes the sun.
		// Commits before the first-person epoch. Under water the engine's blended draw comes first, so the water steps land here instead.
		// Under water DCLF's water surface epoch ends it if it holds work, and the sun water stage takes the rest.
		{ .id = StageId::Sun,
			.owner = Owner::Cs,
			.name = "sun",
			.segment = SegmentId::Sun,
			.names = { "CS stage: sun", "CS stage: sun inputs", "FrameSegments::Sun / " },
			.steps = { {
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.precipitation" } },
				{ .anchor = Anchor::EngineCopy, .clients = { "cs.engine.skeleton.main-copy" } },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.water.surface" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.skeleton.frame@image-space", "cs.engine.imagespace.world.water" } },
				// The post-water composite of the medium into kMAIN, after the water is blended in (ViewAtmosphere's post-water hook).
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.view-atmosphere.composite" } },
				{ .anchor = Anchor::EngineQueries, .clients = { "cs.sky.sun" }, .instanceCount = 2 },
				{ .point = true },
			} },
			.commitBefore = Segments({ Segment::FirstPerson, StageSegment(SegmentId::Post) }),
			// Above water the surface epoch comes before this stage opens, so the stage must not end there while it holds nothing.
			.commitBeforeIfStaged = Segments({ kWaterEpochs[2] }),
			.commit = "just before DCLF's first-person epoch, or RenderWorldPostProcess' entry",
			.commitSite = "BeforeGpuWork, before DCLF's first-person epoch or later GPU work; at the latest RenderWorldPostProcess' entry (Frame/PostChain.cpp)",
			.epochs = { Own(StageSegment(SegmentId::Sun)) } },
		// Sun water: under water, the sun stage's steps after DCLF's water surface epoch ended it. Commits as the sun stage does.
		{ .id = StageId::SunWater,
			.owner = Owner::Cs,
			.name = "sun water",
			.segment = SegmentId::SunWater,
			.names = { "CS stage: sun water", "CS stage: sun water inputs", "FrameSegments::SunWater / " },
			.steps = { {
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.water.surface" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.skeleton.frame@image-space", "cs.engine.imagespace.world.water" } },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.view-atmosphere.composite" } },
				{ .anchor = Anchor::EngineQueries, .clients = { "cs.sky.sun" }, .instanceCount = 2 },
				{ .point = true },
			} },
			.commitBefore = Segments({ Segment::FirstPerson, StageSegment(SegmentId::Post) }),
			.commit = "just before DCLF's first-person epoch, or RenderWorldPostProcess' entry",
			.commitSite = "BeforeGpuWork, before DCLF's first-person epoch or later GPU work; at the latest RenderWorldPostProcess' entry (Frame/PostChain.cpp)",
			.epochs = { Own(StageSegment(SegmentId::SunWater)) } },
		{ .id = StageId::FirstPerson, .owner = Owner::Fragment, .name = "first person", .commit = "Main::RenderFirstPersonView", .epochs = { Covered(Segment::FirstPerson) } },
		// Post: RenderWorldPostProcess (SE 100430, AE 107148). First person's sun query, VL, the pre-tonemap composite, upscale and tonemap.
		// Ends at a point at RenderEndOfFrameEffects' return. The upscaler's evaluate is recorded at execution in both modes.
		{ .id = StageId::Post,
			.owner = Owner::Cs,
			.name = "post",
			.segment = SegmentId::Post,
			.names = { "CS stage: post", "CS stage: post inputs", "FrameSegments::Post / " },
			.steps = { {
				{ .anchor = Anchor::EngineCopy, .clients = { "cs.engine.skeleton.main-copy" } },
				{ .anchor = Anchor::EngineQueries, .clients = { "cs.sky.sun" }, .instanceCount = 2 },
				{ .anchor = Anchor::VolumetricLighting, .clients = { "cs.volumetric-lighting" }, .whole = true },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.post.composites" } },
				{ .anchor = Anchor::PostProcessing, .whole = true },
				{ .anchor = Anchor::Upscale, .clients = { "cs.upscaling", "cs.upscaling.evaluate" }, .whole = true },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.post.hdr" } },
				{ .anchor = Anchor::Tonemap, .clients = { "cs.effects11", "cs.pp" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.post.final", "cs.engine.skeleton.frame@post-chain" } },
				{ .point = true },
			} },
			.commit = "RenderEndOfFrameEffects' return",
			.commitSite = "RenderEndOfFrameEffects' return (Frame/PostChain.cpp)",
			.epochs = { Own(StageSegment(SegmentId::Post)) } },
		// UI, in every frame kind: frame generation's hudless copy and the screenshot's clean scene, screen blood, menu scenes, Scaleform
		// displays, clears at three places, the present's clients, overlay, HDR output and capture. Ends at a point just before the present.
		{ .id = StageId::Ui,
			.owner = Owner::Cs,
			.name = "UI",
			.segment = SegmentId::Ui,
			.names = { "CS stage: UI", "CS stage: UI inputs", "FrameSegments::Ui / " },
			.steps = { {
				// Main_PostProcessing after the post chain: kFRAMEBUFFER without the HUD, with the pre-exposure divided out. Then frame
				// generation's inputs pass, which reads the hudless copy (a whole anchor's taps all reach its first step).
				{ .anchor = Anchor::FrameGeneration, .clients = { "cs.upscaling.hudless", "cs.upscaling.fg-inputs", "cs.screenshot.clean" }, .whole = true },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.blood-splatter" } },
				// The frozen frame, and the book menu's page copies before the displays that draw the pages' text over them.
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.frozen", "cs.engine.imagespace.book" } },
				{ .anchor = Anchor::EngineFrameClear, .clients = { "cs.engine.skeleton.frame@menu-start", "cs.engine.skeleton.shadow@menu-start" } },
				{ .anchor = Anchor::Interface, .clients = { "cs.scaleform" }, .whole = true, .untilLater = true, .firstInstance = 8, .instanceCount = 8 },
				{ .anchor = Anchor::EngineFrameClear, .clients = { "cs.engine.skeleton.frame@menu-scene", "cs.engine.skeleton.shadow@menu-scene" } },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.menu-scenes.menu-shadow" } },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.menu-scenes.menu-depth" } },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.shadow-mask.menu" } },
				{ .anchor = Anchor::EngineDepthCopy, .clients = { "cs.engine.skeleton.depth-copy@ui" } },
				{ .anchor = Anchor::EngineCopy, .clients = { "cs.engine.skeleton.main-copy" } },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.menu-scenes.menu-colour", "cs.menu-scenes.local-map", "cs.menu-scenes.local-map-fog" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.menu" } },
				{ .anchor = Anchor::EngineFrameClear, .clients = { "cs.engine.skeleton.frame@menu-after", "cs.engine.skeleton.shadow@menu-after" } },
				{ .anchor = Anchor::Interface, .clients = { "cs.scaleform" }, .whole = true, .firstInstance = 0, .instanceCount = 8 },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.facetint" } },
				{ .anchor = Anchor::Present, .clients = { "cs.bink", "cs.gpu-load" }, .whole = true },
				{ .anchor = Anchor::Overlay, .clients = { "cs.imgui" }, .whole = true },
				// HDR Display's composite reads the UI texture the steps above drew.
				{ .anchor = Anchor::HDROutput, .clients = { "cs.hdr" }, .whole = true },
				{ .anchor = Anchor::Capture, .clients = { "cs.screenshot" }, .whole = true },
				{ .point = true },
			} },
			.opensLast = true,
			.switchesEverySlot = true,
			.commit = "just before the present",
			.commitSite = "the present hook, after the capture and before the present (Hooks.cpp)",
			.epochs = { Own(StageSegment(SegmentId::Ui)) } },
	} };

	/** @brief The stage of a StageId. */
	constexpr const Stage& StageOf(StageId a_stage) { return kStageTable[static_cast<std::size_t>(a_stage)]; }

	/** @brief How a frame kind matches the frozen world's capture frame. */
	enum class Capture : std::uint8_t
	{
		Any,
		Yes,
		No,
	};

	/** @brief A frame kind's recipe: its stages in frame order. */
	struct KindRecipe
	{
		Kind kind = Kind::Count;
		Capture capture = Capture::Any;
		std::array<StageId, kStages> stages{};
		std::uint32_t count = 0;
	};

	inline constexpr std::array<StageId, kStages> kWorldStages{ StageId::Start, StageId::Shadows, StageId::PreDepth, StageId::Depth, StageId::PreMain, StageId::Opaque,
		StageId::Deferred, StageId::WaterMask, StageId::Water, StageId::WaterSurface, StageId::PostWater, StageId::Forward, StageId::Sun, StageId::SunWater,
		StageId::FirstPerson, StageId::Post, StageId::Ui };

	/** @brief The frame kinds' recipes. A world frame and a frozen world's capture frame have every stage.
	*  Every other kind has the UI stage alone. A stage a kind does not list is absent from its frames. */
	inline constexpr std::array<KindRecipe, 6> kKinds{ {
		{ Kind::World, Capture::Any, kWorldStages, kStages },
		{ Kind::FrozenWorld, Capture::Yes, kWorldStages, kStages },
		{ Kind::FrozenWorld, Capture::No, { StageId::Ui }, 1 },
		{ Kind::MainMenu, Capture::Any, { StageId::Ui }, 1 },
		{ Kind::Loading, Capture::Any, { StageId::Ui }, 1 },
		{ Kind::Bink, Capture::Any, { StageId::Ui }, 1 },
	} };

	/** @brief The recipe of frame kind a_kind (a_capture: a frozen world's capture frame). */
	constexpr const KindRecipe& KindOf(Kind a_kind, bool a_capture)
	{
		for (const auto& kind : kKinds)
			if (kind.kind == a_kind && (kind.capture == Capture::Any || (kind.capture == Capture::Yes) == a_capture))
				return kind;
		return kKinds.back();
	}

	/**
	* @brief Resources a stage reads before the stage that writes them, so it reads the previous frame's contents. Checked by hand.
	* DCLF's stages are exempt, since their colour draws declare every frame texture a colour draw may read. Their Z-prepass draws declare none.
	*/
	// Physical Sky's LUTs, AP shadow, sky tail and cloud outputs: the sky's reflection faces draw in the start stage, before the pre-depth
	// and pre-main stages make this frame's. D3D11 does the same (the faces render before EarlyPrepass and Prepass), and they keep their
	// contents across frames (History/Persistent).
	// IBL's two SH textures: the sky faces (start) read last frame's, since the projection runs at Prepass (pre-main). Both are History.
	// Dynamic Cubemaps' env: IBL's projection (pre-main) reads last frame's, as on D3D11; it is published (deferred+water) and Persistent.
	// Atmosphere and Fog's reflection medium (start) reads the previous frame's multiscatter LUT, Skylighting's probes and Terrain
	// Shadows' heights too, as D3D11's RenderReflection does before EarlyPrepass and Prepass. They are History or Persistent.
	inline constexpr std::array<std::string_view, 32> kPreviousFrame{ "DynamicCubemaps::Env", "DynamicCubemaps::EnvTextureBC6H", "DynamicCubemaps::EnvReflectionsBC6H",
		"Engine::kREFRACTION_NORMALS", "Engine::kUNDERWATER_MASK", "Engine::kWATER_DISPLACEMENT", "Engine::kWATER_DISPLACEMENT_SWAP", "Engine::Texture2D 320x320",
		"Engine::kMAIN_COPY", "IBL::EnvIBL", "IBL::SkyIBL", "TerrainBlending::BlendedDepth16", "Engine::Depth::kMAIN_COPY", "PhysicalSky::TransmittanceLut",
		"PhysicalSky::SkyViewLut", "PhysicalSky::ApLut", "PhysicalSky::DiskPhaseLut", "PhysicalSky::ApShadow",
		"PhysicalSky::SkyTail", "PhysicalSky::ShadowVolume", "PhysicalSky::CloudShadowMean", "PhysicalSky::VolTr", "PhysicalSky::VolLum",
		"PhysicalSky::VolAux", "PhysicalSky::VolCubeTr", "PhysicalSky::VolCubeLum", "PhysicalSky::VolCubeDepth", "PhysicalSky::LightningChannels",
		"PhysicalSky::LightningReflection", "PhysicalSky::MultiscatterLut", "Skylighting::ProbeArray", "TerrainShadows::ShadowHeight" };

	// What is derived from the table.

	constexpr std::uint32_t StepCount(const Stage& a_stage)
	{
		std::uint32_t count = 0;
		while (count < kMaxSteps && !a_stage.steps[count].Empty())
			++count;
		return count;
	}

	constexpr std::uint32_t EpochCount(const Stage& a_stage)
	{
		std::uint32_t count = 0;
		while (count < kMaxEpochs && a_stage.epochs[count].segment != Segment::Count)
			++count;
		return count;
	}

	/** @brief The table's stage of CS stage a_segment (a SegmentId value). */
	constexpr const Stage& CsStage(std::uint32_t a_segment)
	{
		for (const auto& stage : kStageTable)
			if (static_cast<std::uint32_t>(stage.segment) == a_segment)
				return stage;
		return kStageTable.back();
	}

	/** @brief The anchors a CS stage has a step at, a bit each. */
	constexpr std::uint32_t StepAnchors(std::uint32_t a_segment)
	{
		std::uint32_t mask = 0;
		for (const auto& step : CsStage(a_segment).steps)
			if (step.anchor != Anchor::Count)
				mask |= 1u << static_cast<std::uint32_t>(step.anchor);
		return mask;
	}

	/** @brief The anchors a CS stage holds wholly, a bit each. */
	constexpr std::uint32_t WholeAnchors(std::uint32_t a_segment)
	{
		std::uint32_t mask = 0;
		for (const auto& step : CsStage(a_segment).steps)
			if (step.anchor != Anchor::Count && step.whole)
				mask |= 1u << static_cast<std::uint32_t>(step.anchor);
		return mask;
	}

	/** @brief The host's epoch order (FrameAnchors::AddToFrame): every stage's epochs, in frame order. */
	constexpr std::array<Segment, static_cast<std::size_t>(Segment::Count)> Timeline()
	{
		std::array<Segment, static_cast<std::size_t>(Segment::Count)> out{};
		std::size_t n = 0;
		for (const auto& stage : kStageTable)
			for (std::uint32_t e = 0; e < EpochCount(stage) && n < out.size(); ++e)
				out[n++] = stage.epochs[e].segment;
		return out;
	}

	/** @brief Whether the recipe has a fragment source cover a_segment whenever the source draws (Epoch::covered). */
	constexpr bool CoveredSegment(Segment a_segment)
	{
		for (const auto& stage : kStageTable)
			for (std::uint32_t e = 0; e < EpochCount(stage); ++e)
				if (stage.epochs[e].segment == a_segment)
					return stage.epochs[e].covered;
		return false;
	}

	// The table's own checks, at compile time.
	constexpr bool Valid()
	{
		// The stages are in StageId order, and the CS stages in SegmentId order.
		std::uint32_t cs = 0;
		for (std::uint32_t s = 0; s < kStages; ++s) {
			const auto& stage = kStageTable[s];
			if (static_cast<std::uint32_t>(stage.id) != s)
				return false;
			if ((stage.owner == Owner::Cs) != (stage.segment != SegmentId::Count))
				return false;
			if (stage.owner == Owner::Cs) {
				if (static_cast<std::uint32_t>(stage.segment) != cs++)
					return false;
				const auto steps = StepCount(stage);
				// Every anchor step names its clients, and the last step ends the stage.
				if (steps == 0 || (!stage.steps[steps - 1].point && stage.steps[steps - 1].anchor == Anchor::Count))
					return false;
				for (std::uint32_t k = steps; k < kMaxSteps; ++k)
					if (!stage.steps[k].Empty())
						return false;
				// Exactly one epoch, the stage's own.
				if (EpochCount(stage) != 1 || stage.epochs[0].segment != StageSegment(stage.segment))
					return false;
			} else if (StepCount(stage) != 0) {
				return false;
			}
			// Only a fragment source's own epochs are covered.
			for (std::uint32_t e = 0; e < EpochCount(stage); ++e)
				if (stage.epochs[e].covered && stage.owner == Owner::Cs)
					return false;
		}
		if (cs != kSegments)
			return false;
		// An anchor a stage holds wholly has no step in any other stage.
		for (std::uint32_t d = 0; d < kSegments; ++d)
			for (std::uint32_t o = 0; o < kSegments; ++o)
				if (o != d && (WholeAnchors(d) & StepAnchors(o)))
					return false;
		// Within a stage, a whole anchor's every step is whole.
		for (std::uint32_t d = 0; d < kSegments; ++d)
			for (const auto& step : CsStage(d).steps)
				if (step.anchor != Anchor::Count && (WholeAnchors(d) >> static_cast<std::uint32_t>(step.anchor) & 1u) && !step.whole)
					return false;
		// The timeline holds every epoch exactly once.
		const auto timeline = Timeline();
		std::size_t epochs = 0;
		for (const auto& stage : kStageTable)
			epochs += EpochCount(stage);
		if (epochs != timeline.size())
			return false;
		for (std::uint32_t value = 0; value < static_cast<std::uint32_t>(Segment::Count); ++value) {
			std::uint32_t seen = 0;
			for (const auto segment : timeline)
				seen += static_cast<std::uint32_t>(segment) == value ? 1 : 0;
			if (seen != 1)
				return false;
		}
		return true;
	}
	static_assert(Valid(), "the recipe table is inconsistent (RecipeTable.h, Valid)");
}

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
 * @brief The recipe table: CS's whole frame in one constexpr table (docs/development/org-native-frame-graph.md, "The recipe").
 *
 * Frame kinds name their stages in order. A stage is a CS stage (one graph, one ticket) or a Drawcall Limit Fix graph. A CS stage is an
 * ordered list of steps; each step names the client ids it holds. Everything else is derived from this table: FrameAnchors' stage and
 * step lookups, which stages each kind has, the epochs' frame positions for ORG, the host's epoch order, the recipe print and its check.
 *
 * - A **step** is one engine hook in frame order: an anchor (with the clients it holds, by id), another module's epoch (foreign), or a
 *   point that only ends the stage. A whole step holds its anchor wholly: the anchor has no epoch of its own, and no other stage has a
 *   step there.
 * - **Membership is explicit.** A client registers only when a step names it at each anchor its passes use (or, for an anchor that keeps
 *   its own epoch, that epoch's entry names it). Anything else is a recipe mismatch (FrameAnchors::Register). There are no id patterns.
 *   One family registers a variable set of ids: ImageSpace's late clients in guest mode, named below with their serial range.
 * - A stage's **epochs** list, in frame order, every epoch that sits in its part of the frame: its own graph (Graph), an anchor that
 *   keeps its own ticket after it in native mode (Ticket: frame generation, HDR output), and the anchors' own epochs placed there for the
 *   host's epoch order (Placed: the anchors' own epochs, which run only without the stages).
 *
 * Header deliberately free of ORG includes.
 */
namespace RenderGraph::Frame::Recipes::Table
{
	using Anchor = FrameAnchors::Anchor;
	using Segment = RenderGraphRuntime::Segment;

	inline constexpr std::size_t kMaxNames = 8;
	inline constexpr std::size_t kMaxSteps = 19;
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

	/**
	 * @brief A family of clients whose ids carry a serial: prefix + "1" to prefix + count. Not a pattern: the ids are exactly those.
	 */
	struct Family
	{
		std::string_view prefix;
		std::uint16_t count = 0;
	};

	constexpr bool InFamily(const Family& a_family, std::string_view a_id)
	{
		if (a_family.prefix.empty() || a_id.size() <= a_family.prefix.size() || a_id.substr(0, a_family.prefix.size()) != a_family.prefix)
			return false;
		std::uint32_t serial = 0;
		const auto digits = a_id.substr(a_family.prefix.size());
		if (digits.size() > 5 || digits.front() == '0')
			return false;
		for (const char c : digits) {
			if (c < '0' || c > '9')
				return false;
			serial = serial * 10 + static_cast<std::uint32_t>(c - '0');
		}
		return serial >= 1 && serial <= a_family.count;
	}

	/** @brief One step of a CS stage (see the header comment). */
	struct Step
	{
		Anchor anchor = Anchor::Count;     // Count for a foreign or point step
		Names clients{};                   // the clients the step holds at its anchor
		Segment foreign = Segment::Count;  // a foreign step's module epoch
		bool whole = false;                // the stage holds the anchor wholly: it has no epoch of its own
		bool point = false;                // a CS point that is no anchor and no module epoch (TapPoint): it holds no passes and ends the stage
		// The step takes taps only until a later step of its stage has tapped this frame; then its anchor's taps go to the stage's next
		// step at the anchor. The UI stage's displays before a menu's scene: the first display after the scene's first tap is drawn after it.
		bool untilLater = false;
		// The instances of a client with instances (ClientDesc::maxInstances) this step makes and taps: from firstInstance, instanceCount of
		// them (0: every instance from firstInstance). The step's k-th tap this frame is instance firstInstance + k. Two steps at one anchor
		// give the same client disjoint ranges (Scaleform's displays before and after a menu's scene).
		std::uint8_t firstInstance = 0;
		std::uint8_t instanceCount = 0;

		constexpr bool Empty() const { return anchor == Anchor::Count && foreign == Segment::Count && !point; }
	};

	/** @brief What an epoch of a stage's part of the frame is to the recipe. */
	enum class Role : std::uint8_t
	{
		Graph,   // the stage's own graph: a CS stage's epoch, or one of Drawcall Limit Fix's
		Ticket,  // an anchor that keeps its own ticket after the stage in native mode (frame generation, HDR output): in the recipe check
		Placed,  // an anchor's own epoch (or a module's) placed here only for the host's epoch order
	};

	/** @brief An epoch in a stage's part of the frame, with the clients that run on it when it is an anchor's own epoch. */
	struct Epoch
	{
		Segment segment = Segment::Count;
		Role role = Role::Graph;
		Names clients{};  // an anchor's own epoch: the clients that run on it there (without the stages)
		Family family{};  // and a family of them
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
		// The stage's window opens only once every earlier stage of the frame has ended (FrameAnchors' WindowOpen). Without it, an earlier
		// stage that ends at a point and staged nothing yet does not hold the window back.
		bool opensLast = false;
		// A new description switches every frame slot at once, when each slot's recording of it is ready (the UI stage: its description
		// changes with the frame kind, not by frame). Other stages keep one schedule per slot.
		bool switchesEverySlot = false;
		// A point-ended stage commits just before any of these epochs is submitted (FrameAnchors::BeforeGpuWork), if it has not ended yet.
		SegmentList commitBefore = Segments({});
		std::string_view commit;      // where it commits, for the recipe print
		std::string_view commitSite;  // the engine call that commits it
		std::array<Epoch, kMaxEpochs> epochs{};
	};

	constexpr Segment StageSegment(SegmentId a_segment) { return RenderGraphRuntime::StageSegment(static_cast<std::uint32_t>(a_segment)); }

	constexpr Segment AnchorSegment(Anchor a_anchor)
	{
		return static_cast<Segment>(static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass) + static_cast<std::uint32_t>(a_anchor));
	}

	constexpr Epoch Own(Segment a_segment) { return { a_segment, Role::Graph }; }
	constexpr Epoch Placed(Anchor a_anchor, Names a_clients = {}, Family a_family = {}) { return { AnchorSegment(a_anchor), Role::Placed, a_clients, a_family }; }
	constexpr Epoch Ticket(Anchor a_anchor, Names a_clients) { return { AnchorSegment(a_anchor), Role::Ticket, a_clients }; }

	// The epochs whose submission commits the point-ended world stages before their own point: a frame where the usual next GPU work does
	// not come (Drawcall Limit Fix not drawing yet, no first person). Every later stage's epoch, and the epochs past the world's end.
	inline constexpr Segment kEndsWorld[] = { Segment::Forward, Segment::FirstPerson, Segment::AnchorVolumetricLighting, Segment::AnchorPostProcessing,
		Segment::AnchorUpscale, Segment::AnchorInterface, Segment::AnchorPresent, StageSegment(SegmentId::Post) };

	/**
	 * @brief Clients a diagnostic switch registers at every anchor (CS_ORG_ANCHOR_SELFTEST): every whole step holds them, and they may
	 * register at any anchor. Never in a test or play run.
	 */
	inline constexpr Names kDiagnosticClients{ "cs.org.anchor-selftest" };

	/**
	 * @brief The world's stages, in frame order. Recipe order is pass order. Each CS stage's commit point and why it is safe are in the
	 * comment above it (docs/development/org-native-frame-graph.md, "The recipe").
	 */
	inline constexpr std::array<Stage, kStages> kStageTable{ {
		// The frame's start (frame sequence census, a world frame). First the decal tests (SE 35563, AE 36562, before Main::Draw): a tap and
		// an instance each, every instance with its own query range. Then the player's face tint at a load (the session's first world frames:
		// ISAlphaBlend into kPLAYER_FACEGEN_TINT, before the cubemap render). Then Main::Draw's cubemap render (BSCubeMapCamera::RenderCubemap)
		// runs the ReflectionsPrepass hook once, then each reflection face's sky batch (SkyReflections, once per face drawn: the faces' client
		// has an instance per face). The faces are cleared by the load operations of their sky's first draw; the engine's clears of them are
		// dropped. Main::RenderWaterEffects runs after the faces: the water displacement update and the flow copies are the start client's
		// steps at the ImageSpace anchor (ImageSpace's StartSteps.inl), a tap per update and per copy, each with its own instances. Last, the
		// shadow maps' clears (BSShadowLight::RenderShadowmap) that Drawcall Limit Fix's shadow view does not host: every world frame before
		// DCLF draws and during its load holds. They are passes at a place (Engine/Skeleton): the engine's call stages each one. Pre-depth,
		// pre-main and post read those maps. Its commit is a point just before the next GPU work that follows it in every world frame is
		// submitted: DCLF's shadow view epoch or a later stage (BeforeGpuWork), at the latest when Main::RenderShadowMaps has returned (the
		// EarlyPrepass hook's start, before its D3D11 work).
		{ .id = StageId::Start,
			.owner = Owner::Cs,
			.name = "start",
			.segment = SegmentId::Start,
			.names = { "CS stage: start", "CS stage: start inputs", "FrameSegments::Start / ", "cs.segment.start" },
			.steps = { {
				{ .anchor = Anchor::DecalQueries, .clients = { "cs.decals.occlusion" }, .whole = true },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.facetint" } },
				{ .anchor = Anchor::ReflectionsPrepass, .clients = { "cs.cloud-shadows", "cs.cloud-shadows.layers" }, .whole = true },
				{ .anchor = Anchor::SkyReflections, .clients = { "cs.sky.faces", "cs.cloud-shadows.layers" }, .whole = true },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.start" } },
				{ .anchor = Anchor::EngineShadowClear, .clients = { "cs.engine.skeleton.shadow@start" } },
				{ .point = true },
			} },
			.commitBefore = Segments({ Segment::ShadowView, Segment::SkyOcclusion, Segment::ZPrepass, Segment::FirstPersonDepth, Segment::LightCulling, Segment::MainOpaque,
				StageSegment(SegmentId::PreDepth), StageSegment(SegmentId::PreMain), StageSegment(SegmentId::Deferred), kEndsWorld[0], kEndsWorld[1], kEndsWorld[2],
				kEndsWorld[3], kEndsWorld[4], kEndsWorld[5], kEndsWorld[6], kEndsWorld[7] }),
			.commit = "just before DCLF's shadow view epoch, or Main::RenderShadowMaps' return",
			.commitSite = "BeforeGpuWork, before DCLF's shadow view epoch or later GPU work; at the latest the EarlyPrepass hook's start (Deferred::EarlyPrepasses)",
			.epochs = { Own(StageSegment(SegmentId::Start)), Placed(Anchor::DecalQueries), Placed(Anchor::EngineShadowClear, { "cs.engine.skeleton.shadow" }) } },
		{ .id = StageId::Shadows,
			.owner = Owner::Dclf,
			.name = "shadows",
			.commit = "Drawcall Limit Fix's shadow views",
			.epochs = { Own(Segment::ShadowView), Own(Segment::SkyOcclusion), Placed(Anchor::EarlyPrepass) } },
		// The pre-depth stage: the EarlyPrepass clients (Main::RenderShadowMaps returned), then Main::RenderDepth's clear of the main depth when
		// Drawcall Limit Fix's Z-prepass does not host it (every world frame before DCLF draws and during its load holds; no pre-depth pass
		// reads the main depth). Its commit is a point just before DCLF's Z-prepass or a later stage (BeforeGpuWork, and a later stage's first
		// tap): the clear must reach the GPU before the Z-prepass draws, which a load hold's frames submit.
		{ .id = StageId::PreDepth,
			.owner = Owner::Cs,
			.name = "pre-depth",
			.segment = SegmentId::PreDepth,
			.names = { "CS segment: pre-depth", "CS segment: pre-depth inputs", "FrameSegments::PreDepth / ", "cs.segment.pre-depth" },
			.steps = { {
				{ .anchor = Anchor::EarlyPrepass,
					.clients = { "cs.cloud-shadows.layers", "cs.grass-collision", "cs.llf.local-shadows", "cs.terrain-blending.terrain-depth-clear", "cs.terrain-shadows",
						"cs.true-pbr.glints", "cs.volumetric-shadows" },
					.whole = true },
				{ .anchor = Anchor::EngineFrameClear, .clients = { "cs.engine.skeleton.frame@pre-depth" } },
				{ .point = true },
			} },
			.commitBefore = Segments({ Segment::ZPrepass, Segment::FirstPersonDepth, Segment::Reflection, Segment::LightCulling, Segment::MainOpaque, StageSegment(SegmentId::PreMain),
				StageSegment(SegmentId::Deferred), kEndsWorld[0], kEndsWorld[1], kEndsWorld[2], kEndsWorld[3], kEndsWorld[4], kEndsWorld[5], kEndsWorld[6], kEndsWorld[7] }),
			.commit = "just before DCLF's Z-prepass, or Main::RenderDepth's depth copy",
			.commitSite = "BeforeGpuWork, before DCLF's Z-prepass or later GPU work; at the latest Main::RenderDepth's depth copy (Engine/Skeleton, RunCopy)",
			.epochs = { Own(StageSegment(SegmentId::PreDepth)), Placed(Anchor::EngineFrameClear, { "cs.engine.skeleton.frame" }) } },
		// Drawcall Limit Fix's water reflection faces come after the Z-prepass (TESWaterReflections::Update).
		{ .id = StageId::Depth,
			.owner = Owner::Dclf,
			.name = "depth",
			.commit = "Main::RenderDepth",
			.epochs = { Own(Segment::ZPrepass), Own(Segment::FirstPersonDepth), Placed(Anchor::EngineDepthCopy), Placed(Anchor::DepthDone),
				Placed(Anchor::EngineCubeClear, { "cs.engine.skeleton.cube" }), Own(Segment::Reflection), Placed(Anchor::ReflectionsPrepass), Placed(Anchor::SkyReflections),
				{ Segment::LightCulling, Role::Placed } } },
		// Main::RenderDepth's end to the main pass's sky. The depth copy comes after DCLF's Z-prepass epoch (which holds first person's depth
		// too), and light culling, the Prepass clients and the main pass's sky batch come before DCLF's main opaque epoch: no other GPU work runs
		// in between (the census counts any that does). The sky's batch does not run in every world frame, so the stage ends at a point: just
		// before DCLF's main opaque epoch or a later stage is submitted (BeforeGpuWork, and a later stage's first tap).
		{ .id = StageId::PreMain,
			.owner = Owner::Cs,
			.name = "pre-main",
			.segment = SegmentId::PreMain,
			.names = { "CS segment: pre-main", "CS segment: pre-main inputs", "FrameSegments::PreMain / ", "cs.segment.pre-main" },
			.steps = { {
				{ .anchor = Anchor::EngineDepthCopy, .clients = { "cs.engine.skeleton.depth-copy" } },
				{ .anchor = Anchor::DepthDone, .clients = { "cs.terrain-blending" }, .whole = true },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.engine.skeleton.frame@engine-draws", "cs.shadow-mask.world" } },
				{ .foreign = Segment::LightCulling },
				{ .anchor = Anchor::Prepass,
					.clients = { "cs.deferred.clears", "cs.engine.skeleton.frame@prepass", "cs.exp-height-fog", "cs.ibl", "cs.screen-space-shadows", "cs.skylighting" },
					.whole = true },
				{ .anchor = Anchor::Sky, .clients = { "cs.sky.main" }, .whole = true },
				{ .point = true },
			} },
			.commitBefore = Segments({ Segment::MainOpaque, StageSegment(SegmentId::Deferred), kEndsWorld[0], kEndsWorld[1], kEndsWorld[2], kEndsWorld[3], kEndsWorld[4],
				kEndsWorld[5], kEndsWorld[6], kEndsWorld[7] }),
			.commit = "just before DCLF's main opaque epoch (the main pass's sky)",
			.commitSite = "BeforeGpuWork, before DCLF's main opaque epoch or later GPU work; at the latest CS's depth copy in Deferred::DeferredPasses (Engine/Skeleton, RunCopy)",
			.epochs = { Own(StageSegment(SegmentId::PreMain)), Placed(Anchor::Prepass), Placed(Anchor::Sky) } },
		{ .id = StageId::Opaque, .owner = Owner::Dclf, .name = "opaque", .commit = "the main pass's opaque batches", .epochs = { Own(Segment::MainOpaque) } },
		// Deferred::StartDeferred, after DCLF's main opaque epoch: the deferred passes (with CS's depth copy they host), the composite and the
		// end of the deferred passes; then the rest of Main::RenderWorld (100424 / 107142): the water stencil's clear and the water mask
		// (FinishAccumulating's stencil batch), the reflections, RenderSSAO's two calls with the snow pass or its copy between them, the
		// engine's kMAIN copy, the water, the water targets' clear and the water blend. Its commit is a point just before DCLF's forward epoch
		// is submitted (BeforeGpuWork), the next GPU work after the water blend: every tap has staged by then. The water's draws and the kMAIN
		// copy are gated: a step that does not fire this frame (no water in view) does nothing.
		{ .id = StageId::Deferred,
			.owner = Owner::Cs,
			.name = "deferred+water",
			.segment = SegmentId::Deferred,
			.names = { "CS stage: deferred+water", "CS stage: deferred+water inputs", "FrameSegments::Deferred / ", "cs.segment.deferred" },
			.steps = { {
				{ .anchor = Anchor::DeferredPasses,
					.clients = { "cs.dyncubemaps", "cs.engine.skeleton.depth-copy@deferred-passes", "cs.org.graphres-selftest", "cs.ssgi", "cs.ssgi.reset", "cs.subsurface" },
					.whole = true },
				{ .anchor = Anchor::DeferredComposite, .clients = { "cs.deferred" }, .whole = true },
				{ .anchor = Anchor::PostComposite, .clients = { "cs.e11-rays" }, .whole = true },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.engine.skeleton.frame@engine-draws", "cs.water.mask" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.world.scopes" } },
				{ .anchor = Anchor::EngineCopy, .clients = { "cs.engine.skeleton.main-copy" } },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.water.surface" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.skeleton.frame@image-space", "cs.engine.imagespace.world.water" } },
				{ .point = true },
			} },
			.commitBefore = Segments({ kEndsWorld[0], kEndsWorld[1], kEndsWorld[2], kEndsWorld[3], kEndsWorld[4], kEndsWorld[5], kEndsWorld[6], kEndsWorld[7] }),
			.commit = "the engine's blended draw (just before DCLF's forward epoch)",
			.commitSite = "the engine's blended draw for the main accumulator (Features/Forward's ForwardHooks.cpp), else BeforeGpuWork",
			.epochs = { Own(StageSegment(SegmentId::Deferred)), Placed(Anchor::DeferredPasses), Placed(Anchor::DeferredComposite), Placed(Anchor::PostComposite),
				Placed(Anchor::EngineDraws) } },
		{ .id = StageId::Forward, .owner = Owner::Dclf, .name = "forward", .commit = "the engine's blended draw", .epochs = { Own(Segment::Forward) } },
		// The sun stage: the rest of Main::RenderWorld after DCLF's forward epoch. First the precipitation: the engine draws its rain and snow
		// in the main pass's blended draws (FinishAccumulatingPostResolveDepth's blended draw, the grouped alphas), and DCLF's forward epoch is
		// submitted at that draw's start, before the engine's own blended draws (Features/Forward's ForwardHooks.cpp). So each precipitation
		// draw comes after the deferred+water stage's commit, and before the sun query. Then the sun occlusion query
		// (Sun::SubmitOcclusionQuery, SE 0x1403bbf20), which must run on the GPU before the first-person geometry, which would occlude the
		// sun: a stage of its own between DCLF's forward and first-person graphs, committed just before the first-person epoch is submitted
		// (BeforeGpuWork) or at a later stage's first tap. The sun's client has two instances (the world's and first person's query); a stage
		// run's k-th tap is instance k. The precipitation's draws are frame inputs, recorded in the stage's late batch. The deferred+water
		// stage commits at the blended draw in every world frame (Features/Forward's hook), not only when DCLF's forward epoch runs, so this
		// window is open for the sun's query in a frame before DCLF draws too. Under water the engine's blended draw comes first in
		// FinishAccumulatingPostResolveDepth (its call at +0x51, before the water), so the deferred+water stage commits before the water: the
		// kMAIN copy, the water's draws, the water targets' clears and the water blend are this stage's steps then (the same clients as the
		// deferred+water stage's, after the precipitation). Above water that stage takes them, and these steps skip.
		{ .id = StageId::Sun,
			.owner = Owner::Cs,
			.name = "sun",
			.segment = SegmentId::Sun,
			.names = { "CS stage: sun", "CS stage: sun inputs", "FrameSegments::Sun / ", "cs.segment.sun" },
			.steps = { {
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.precipitation" } },
				{ .anchor = Anchor::EngineCopy, .clients = { "cs.engine.skeleton.main-copy" } },
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.water.surface" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.skeleton.frame@image-space", "cs.engine.imagespace.world.water" } },
				{ .anchor = Anchor::EngineQueries, .clients = { "cs.sky.sun" }, .instanceCount = 2 },
				{ .point = true },
			} },
			.commitBefore = Segments({ Segment::FirstPerson, Segment::AnchorVolumetricLighting, Segment::AnchorPostProcessing, Segment::AnchorUpscale, Segment::AnchorInterface,
				Segment::AnchorPresent, StageSegment(SegmentId::Post) }),
			.commit = "just before DCLF's first-person epoch, or RenderWorldPostProcess' entry",
			.commitSite = "BeforeGpuWork, before DCLF's first-person epoch or later GPU work; at the latest RenderWorldPostProcess' entry (Frame/PostChain.cpp)",
			.epochs = { Own(StageSegment(SegmentId::Sun)), Placed(Anchor::EngineQueries) } },
		{ .id = StageId::FirstPerson, .owner = Owner::Dclf, .name = "first person", .commit = "Main::RenderFirstPersonView", .epochs = { Own(Segment::FirstPerson) } },
		// The post stage: ImageSpaceManager::RenderWorldPostProcess (SE 100430, AE 107148). First the first person's sun query
		// (Main::RenderFirstPersonView, after DCLF's first-person epoch), as the sun's client's instance 0 of this stage's run. The volumetric
		// lighting (whole: its hook runs only there), the pre-tonemap composite as CS passes (ChainSteps.inl), the PostProcessing anchor
		// (reserved so its hook always runs; no client), the Upscale anchor (the upscaler's evaluate as a pass recorded at execution, the copy
		// into kMAIN, the depth upscale), RenderEndOfFrameEffects (99023 / 105674) as CS passes split at the Tonemap anchor (its passes replace
		// or follow the tonemap draw, so they sit between the HDR effect's draws and the tonemap's), and its return as the end (a point: no
		// anchor runs there). Nothing else submits GPU work between the first tap and the end (the census counts any that does). Before the
		// query, the engine's kMAIN copy that Main::RenderFirstPersonView makes after DCLF's first-person epoch in some cells (every frame on
		// Solstheim, the worldspace run). Frame generation's hudless copy keeps its anchor's ticket (frame generation is off in native mode).
		// The upscaler's evaluate is a pass recorded at execution in both modes: in guest it records into DXVK's command buffer inside the
		// stage's block (ORGUpscaling's EvaluatePass).
		{ .id = StageId::Post,
			.owner = Owner::Cs,
			.name = "post",
			.segment = SegmentId::Post,
			.names = { "CS stage: post", "CS stage: post inputs", "FrameSegments::Post / ", "cs.segment.post" },
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
			.epochs = { Own(StageSegment(SegmentId::Post)), Placed(Anchor::VolumetricLighting), Placed(Anchor::PostProcessing), Placed(Anchor::Upscale),
				Placed(Anchor::ImageSpace, { "cs.engine.imagespace" }, { "cs.engine.imagespace.late.", 64 }), Placed(Anchor::EngineCopy), Placed(Anchor::Tonemap),
				Ticket(Anchor::FrameGeneration, { "cs.upscaling.hudless" }), Placed(Anchor::Interface), Placed(Anchor::Present) } },
		// The UI stage, in every frame kind's recipe. The engine draws, in this order (frame sequence census): the screen blood (a world frame's
		// Main::RenderScreenSplatter, which Main::Draw calls after RenderWorldPostProcess returned, so after the post stage's end:
		// docs/development/p5/blood-splatter.md), the frozen frame's composite (Main::Render; a menu over a frozen world only, which has no
		// screen blood), the frame's first clears, displays, the shadow map's and a menu scene's clears, the scene's depth (Drawcall Limit
		// Fix's menu depth batch), the shadow mask, the depth copy and the kMAIN copy, the scene's colour, its composite (UI3DSceneManager::
		// Render's end-of-frame call, ImageSpace's MenuSteps.inl), the main depth's clear again, more displays, the player's face tint at a
		// load (in a loading screen's frame), the present's clients, the overlay and the capture. A world frame's menu scene (an inventory
		// item model, the local map) comes after the post stage, between the menu's displays, and takes the same steps.
		// - Clears are passes at three places, each a client of the frame and shadow clear groups (Engine/Skeleton): the stage's start, the
		//   scene's start and after the composite. The engine clears the main depth three times a loading screen's frame and kMAIN twice; the
		//   engine's call stages each clear into the place it comes at.
		// - Scaleform's displays: each display segment is a tap and an instance of the Scaleform client, its draw list a frame input recorded
		//   in the late batch. The displays before a scene are instances 8 to 15; once a later step tapped, they are instances 0 to 7, after
		//   the scene. A frame without a scene draws every display as instances 8 to 15.
		// - The screen blood's draw list, the scene's draw lists, the composites' constants, a Bink movie's tiles (queued by the movie's tick)
		//   and the capture (native mode's capture point is just before the stage's commit, Hooks.cpp) are frame inputs, recorded in the
		//   stage's late batch. What a frame does not use skips at execution, its barriers folded.
		// - The depth copy is the stage's own gated client (cs.engine.skeleton.depth-copy@ui): the pre-main stage copies the main depth in
		//   every world frame, this one only when the engine's call asks.
		// It ends at a point: just before the present (Hooks.cpp), or before HDR output, which follows the overlay on the GPU and keeps its
		// anchor's ticket. (A Bink frame's clear of the main depth comes before its Begin and has no reader: dropped, Engine/Skeleton.)
		{ .id = StageId::Ui,
			.owner = Owner::Cs,
			.name = "UI",
			.segment = SegmentId::Ui,
			.names = { "CS stage: UI", "CS stage: UI inputs", "FrameSegments::Ui / ", "cs.segment.ui" },
			.steps = { {
				{ .anchor = Anchor::EngineDraws, .clients = { "cs.blood-splatter" } },
				{ .anchor = Anchor::ImageSpace, .clients = { "cs.engine.imagespace.frozen" } },
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
				{ .anchor = Anchor::Present, .clients = { "cs.bink", "cs.gpu-load", "cs.org.anchor-selftest-fail" }, .whole = true },
				{ .anchor = Anchor::Overlay, .clients = { "cs.imgui" }, .whole = true },
				{ .anchor = Anchor::Capture, .clients = { "cs.screenshot" }, .whole = true },
				{ .point = true },
			} },
			.opensLast = true,
			.switchesEverySlot = true,
			.commitBefore = Segments({ Segment::AnchorHDROutput, Segment::AnchorCapture }),
			.commit = "just before the present (or HDR output)",
			.commitSite = "the present hook, before the capture and the present (Hooks.cpp); BeforeGpuWork before HDR output or the Capture anchor",
			.epochs = { Own(StageSegment(SegmentId::Ui)), Placed(Anchor::Overlay), Ticket(Anchor::HDROutput, { "cs.hdr" }), Placed(Anchor::Capture) } },
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
		StageId::Deferred, StageId::Forward, StageId::Sun, StageId::FirstPerson, StageId::Post, StageId::Ui };

	/**
	 * @brief The frame kinds' recipes (docs/development/org-native-frame-graph.md, "Frame kinds and the UI stage"). The world, and a frozen
	 * world's capture frame, which draws the world once: every stage. Every other frame (a menu over a frozen world after its capture frame,
	 * the main menu and any frame before the player has 3D, a loading screen's frame, a Bink movie's) has the UI stage alone. It holds, in
	 * the engine's order: the frozen frame's composite, the first clears, the displays before a menu's scene, the scene's clears, its
	 * depth, shadow mask, depth and kMAIN copies, colour and composite, the main depth's last clear, the displays after the scene, the
	 * present's clients (a Bink movie's tiles), the overlay and the capture. A CS stage a kind does not list is absent from its frames.
	 */
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
	 * @brief Resources a stage reads before the stage that first writes them in the frame: the previous frame's contents, each checked by
	 * hand against the stage that writes it. Exact graph resource names.
	 * - written by forward or post and read earlier in the next frame: the refraction normals, the underwater mask, the water displacement
	 *   pair and the water flow map (the water surface), kMAIN_COPY (the water surface);
	 * - written by the deferred stages and read by the start stage's face sky: the sky's IBL and Terrain Blending's blended depth;
	 * - the depth copy Volumetric Lighting makes in post, read by the menu scenes' local map fog (the UI stage);
	 * - the dynamic cubemaps the deferred stage compresses, read by the start stage's face sky.
	 * Drawcall Limit Fix's stages are not held to it: their graphs declare the whole binding table their shaders may read (the light lists,
	 * the shadow mask, the luminance chain), so they "read" much that CS writes later; the check counts those apart.
	 */
	inline constexpr std::array<std::string_view, 11> kPreviousFrame{ "DynamicCubemaps::EnvTextureBC6H", "DynamicCubemaps::EnvReflectionsBC6H",
		"Engine::kREFRACTION_NORMALS", "Engine::kUNDERWATER_MASK", "Engine::kWATER_DISPLACEMENT", "Engine::kWATER_DISPLACEMENT_SWAP", "Engine::Texture2D 320x320",
		"Engine::kMAIN_COPY", "IBL::SkyIBL", "TerrainBlending::BlendedDepth16", "Engine::Depth::kMAIN_COPY" };

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

	/** @brief The anchors a CS stage holds wholly, a bit each: they have no epoch of their own. */
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
				// Every anchor step names its clients (a whole step with no client is a hook that only ends nothing: the PostProcessing anchor),
				// and the last step ends the stage.
				if (steps == 0 || (!stage.steps[steps - 1].point && stage.steps[steps - 1].anchor == Anchor::Count))
					return false;
				for (std::uint32_t k = steps; k < kMaxSteps; ++k)
					if (!stage.steps[k].Empty())
						return false;
				// Exactly one Graph epoch, the stage's own.
				std::uint32_t own = 0;
				for (std::uint32_t e = 0; e < EpochCount(stage); ++e)
					if (stage.epochs[e].role == Role::Graph)
						own += stage.epochs[e].segment == StageSegment(stage.segment) ? 1 : 100;
				if (own != 1)
					return false;
			} else if (StepCount(stage) != 0) {
				return false;
			}
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

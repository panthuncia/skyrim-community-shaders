#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/FrameAnchors.h"

#	include "Globals.h"
#	include "RenderGraph/Frame/FrameDescription.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Frame/Recipe.h"
#	include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#	include "RenderGraph/Engine/Native/Native.h"
#	include "RenderGraph/Engine/Targets/EngineTargets.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/BuildManager.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/ExternalWork.h"
#	include "RenderGraph/FrameSequence.h"
#	include "RenderGraph/FrameStaging.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/RunShapes.h"
#	include "RenderGraph/SharedConstants.h"
#	include "RenderGraph/Switches.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/PassExecutionContext.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/RenderGraphPass.h>

#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <Resources/TextureDescription.h>
#	include <Resources/VersionedTexture.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <bit>
#	include <chrono>
#	include <thread>
#	include <condition_variable>
#	include <map>
#	include <mutex>
#	include <optional>
#	include <typeindex>

namespace FrameAnchors
{
	namespace
	{
		using Segment = RenderGraphRuntime::Segment;

		constexpr std::uint32_t kAnchors = static_cast<std::uint32_t>(Anchor::Count);
		static_assert(static_cast<std::uint32_t>(Segment::AnchorEngineCopy) - static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass) + 1 == kAnchors,
			"one segment per anchor, in the anchors' order");
		static_assert(static_cast<std::uint32_t>(Segment::Count) <= 64, "TakeSubmittedSegments holds one bit per segment");
		static_assert(kAnchors <= 32, "anchor masks are 32-bit");

		constexpr const char* kExtensionId = "cs.org.frame-anchors";

		struct AnchorInfo
		{
			const char* name;
			SegmentNames names;
			bool late;  // may be the last graph work before an upscaler, frame generation or the present
		};

		constexpr std::array<AnchorInfo, kAnchors> kInfo{ {
			// Runs a frame: one per cubemap the engine renders (BSCubeMapCamera::RenderCubemap)
			{ "ReflectionsPrepass", { "CS anchor: reflections prepass", "CS anchor: reflections prepass inputs", "FrameAnchors::ReflectionsPrepass / ", "cs.anchor.reflections-prepass" }, false },
			// Runs a frame: one per reflection face whose sky is drawn
			{ "SkyReflections", { "CS anchor: reflection sky", "CS anchor: reflection sky inputs", "FrameAnchors::SkyReflections / ", "cs.anchor.sky-reflections" }, false },
			{ "EarlyPrepass", { "CS anchor: early prepass", "CS anchor: early prepass inputs", "FrameAnchors::EarlyPrepass / ", "cs.anchor.early-prepass" }, false },
			{ "DepthDone", { "CS anchor: depth done", "CS anchor: depth done inputs", "FrameAnchors::DepthDone / ", "cs.anchor.depth-done" }, false },
			{ "Prepass", { "CS anchor: prepass", "CS anchor: prepass inputs", "FrameAnchors::Prepass / ", "cs.anchor.prepass" }, false },
			{ "Sky", { "CS anchor: sky", "CS anchor: sky inputs", "FrameAnchors::Sky / ", "cs.anchor.sky" }, false },
			{ "DeferredPasses", { "CS anchor: deferred passes", "CS anchor: deferred passes inputs", "FrameAnchors::DeferredPasses / ", "cs.anchor.deferred-passes" }, false },
			{ "DeferredComposite", { "CS anchor: deferred composite", "CS anchor: deferred composite inputs", "FrameAnchors::DeferredComposite / ", "cs.anchor.deferred-composite" }, false },
			{ "PostComposite", { "CS anchor: post-composite", "CS anchor: post-composite inputs", "FrameAnchors::PostComposite / ", "cs.anchor.post-composite" }, false },
			{ "PostProcessing", { "CS anchor: post-processing", "CS anchor: post-processing inputs", "FrameAnchors::PostProcessing / ", "cs.anchor.post-processing" }, true },
			{ "Upscale", { "CS anchor: upscale", "CS anchor: upscale inputs", "FrameAnchors::Upscale / ", "cs.anchor.upscale" }, true },
			{ "Tonemap", { "CS anchor: tonemap", "CS anchor: tonemap inputs", "FrameAnchors::Tonemap / ", "cs.anchor.tonemap" }, true },
			{ "FrameGeneration", { "CS anchor: frame generation", "CS anchor: frame generation inputs", "FrameAnchors::FrameGeneration / ", "cs.anchor.frame-generation" }, true },
			{ "Present", { "CS anchor: present", "CS anchor: present inputs", "FrameAnchors::Present / ", "cs.anchor.present" }, true },
			{ "Overlay", { "CS anchor: overlay", "CS anchor: overlay inputs", "FrameAnchors::Overlay / ", "cs.anchor.overlay" }, true },
			{ "HDROutput", { "CS anchor: HDR output", "CS anchor: HDR output inputs", "FrameAnchors::HDROutput / ", "cs.anchor.hdr-output" }, true },
			{ "Capture", { "CS anchor: capture", "CS anchor: capture inputs", "FrameAnchors::Capture / ", "cs.anchor.capture" }, true },
			// Runs a frame: one per shadow map slice cleared
			{ "EngineShadowClear", { "CS anchor: engine shadow clear", "CS anchor: engine shadow clear inputs", "FrameAnchors::EngineShadowClear / ", "cs.anchor.engine-shadow-clear" }, false },
			// Runs a frame: one per reflection face batch
			{ "EngineCubeClear", { "CS anchor: engine cube clear", "CS anchor: engine cube clear inputs", "FrameAnchors::EngineCubeClear / ", "cs.anchor.engine-cube-clear" }, false },
			// Runs a frame: one per batch of the other engine clears
			{ "EngineFrameClear", { "CS anchor: engine frame clear", "CS anchor: engine frame clear inputs", "FrameAnchors::EngineFrameClear / ", "cs.anchor.engine-frame-clear" }, false },
			// Runs a frame: Main::RenderDepth, FinishAccumulating and DeferredPasses
			{ "EngineDepthCopy", { "CS anchor: engine depth copy", "CS anchor: engine depth copy inputs", "FrameAnchors::EngineDepthCopy / ", "cs.anchor.engine-depth-copy" }, false },
			// Runs a frame: one per Scaleform segment drawn on the graph (a display, and one more per D3D11 part); 6 was not passed in a menu tour
			{ "Interface", { "CS anchor: interface", "CS anchor: interface inputs", "FrameAnchors::Interface / ", "cs.anchor.interface" }, true },
			// Runs a frame: one per batch of ImageSpace draws and dispatches, the sum of ImageSpace.cpp's kScopeRuns: RenderSSAO's, the
			// reflection pass's, the HDR effect's and the pre-tonemap composites' (two each: a batch cut short by a draw the graph does
			// not take), one per effect call outside them (depth of field or the distant blur, its two compute blurs, refraction, the
			// radial blur, the snow copy, the water blend, the LDR downsample, the copies, the blurs and the get-hit draw there:
			// twenty-four), and one per water flow copy (four).
			// Late: the TAA or the upscaler can follow a batch inside the chain.
			{ "ImageSpace", { "CS anchor: image space", "CS anchor: image space inputs", "FrameAnchors::ImageSpace / ", "cs.anchor.image-space" }, true },
			// Runs a frame: the world's shadow masks, the water mask, the water, a menu scene's shadow masks and the screen blood.
			{ "EngineDraws", { "CS anchor: engine draws", "CS anchor: engine draws inputs", "FrameAnchors::EngineDraws / ", "cs.anchor.engine-draws" }, true },
			// Runs a frame: one per sun occlusion query the engine submits (Sun::SubmitOcclusionQuery's indices 0 to 2; two seen a frame).
			{ "EngineQueries", { "CS anchor: engine queries", "CS anchor: engine queries inputs", "FrameAnchors::EngineQueries / ", "cs.anchor.engine-queries" }, false },
			// Runs a frame: one, at the VL blur's call in RenderWorldPostProcess (the generate, the raymarch, the apply and the blurs)
			{ "VolumetricLighting", { "CS anchor: volumetric lighting", "CS anchor: volumetric lighting inputs", "FrameAnchors::VolumetricLighting / ", "cs.anchor.volumetric-lighting" }, false },
			// Runs a frame: one per decal occlusion test (the decals the engine places this frame; none in most frames)
			{ "DecalQueries", { "CS anchor: decal queries", "CS anchor: decal queries inputs", "FrameAnchors::DecalQueries / ", "cs.anchor.decal-queries" }, false },
			// Runs a frame: one per Bink tick inside another frame (player mode 2; none in most frames)
			{ "MovieTiles", { "CS anchor: movie tiles", "CS anchor: movie tiles inputs", "FrameAnchors::MovieTiles / ", "cs.anchor.movie-tiles" }, true },
			// Runs a frame: one per copy of kMAIN into its copy texture the engine makes (native mode only)
			{ "EngineCopy", { "CS anchor: engine copy", "CS anchor: engine copy inputs", "FrameAnchors::EngineCopy / ", "cs.anchor.engine-copy" }, true },
		} };

		// Where each segment runs in a frame: AddToFrame puts an anchor before the first epoch of the order that runs after it. A
		// segment's place only has to be right against the anchors next to it; the order names the frame,
		// while the ledgers order the work whatever runs.
		constexpr std::array<Segment, static_cast<std::size_t>(Segment::Count)> kTimeline{
			Segment::DeclaredStart,            // the start stage (SegmentsOn): it commits just before DCLF's shadow view epoch
			Segment::AnchorDecalQueries,       // the decal occlusion tests, at the frame's start before Main::Draw
			Segment::AnchorEngineShadowClear,  // each shadow map slice's clear, before its draws (nominally here)
			Segment::ShadowView,
			Segment::SkyOcclusion,
			Segment::AnchorEarlyPrepass,      // Main::RenderShadowMaps returned
			Segment::DeclaredPreDepth,        // the declared pre-depth segment (SegmentsOn): its tap is EarlyPrepass's hook
			Segment::AnchorEngineFrameClear,  // the main depth's clear in Main::RenderDepth (and the frame's other clears)
			Segment::ZPrepass,                // Main::RenderDepth
			Segment::FirstPersonDepth,
			Segment::AnchorEngineDepthCopy,  // Main::RenderDepth's end (and the later copies of the same depth)
			Segment::AnchorDepthDone,        // Main::RenderDepth returned, with the first-person depth
			Segment::AnchorEngineCubeClear,  // each reflection face's clear, before its draws (nominally here)
			Segment::Reflection,
			Segment::AnchorReflectionsPrepass,  // the cubemap render, nominally here (zero or more times a frame)
			Segment::AnchorSkyReflections,      // its faces' sky, once per face drawn
			Segment::LightCulling,              // Light Limit Fix's Prepass
			Segment::DeclaredPreMain,           // the declared pre-main segment (SegmentsOn): its end tap is the Prepass anchor's hook
			Segment::AnchorPrepass,             // after every feature's Prepass
			Segment::AnchorSky,                 // the main pass's sky batch, before Drawcall Limit Fix's colour epoch
			Segment::MainOpaque,
			Segment::DeclaredDeferred,         // the deferred+water stage (SegmentsOn): it commits just before DCLF's forward epoch
			Segment::AnchorDeferredPasses,     // EndDeferred, after the blended decals
			Segment::AnchorDeferredComposite,  // DeferredPasses, after the D3D11 SSGI and SSS
			Segment::AnchorPostComposite,      // DeferredPasses' end, after the composite
			Segment::AnchorEngineDraws,        // the water, before the forward epoch (nominally here: the shadow masks and the water mask run before, the blood after)
			Segment::Forward,
			Segment::DeclaredSun,          // the sun stage (SegmentsOn): the world's sun query, committed just before DCLF's first-person epoch
			Segment::AnchorEngineQueries,  // the sun's occlusion queries, after Main::RenderWorld's accumulator (nominally here: a second runs later in the frame)
			Segment::FirstPerson,
			Segment::DeclaredPost,              // the post stage (SegmentsOn): its end is RenderEndOfFrameEffects' return
			Segment::AnchorVolumetricLighting,  // RenderWorldPostProcess's VL apply and blur, before the composites and Main_PostProcessing
			Segment::AnchorPostProcessing,      // Main_PostProcessing, before the upscaler
			Segment::AnchorUpscale,             // after the upscaler's evaluate
			Segment::AnchorImageSpace,          // the ImageSpace chain's batches (pre-tonemap composites, the HDR effect), a few times a frame
			Segment::AnchorEngineCopy,          // the engine's kMAIN copy, between the chain's batches (nominally here; native mode only)
			Segment::AnchorTonemap,             // in the ImageSpace chain after it
			Segment::AnchorFrameGeneration,     // after the ImageSpace chain, before frame generation
			Segment::AnchorMovieTiles,          // a Bink movie inside another frame, at its tile draw (nominally here: the frame's UI time)
			Segment::AnchorInterface,           // Scaleform's displays (UI::DrawInterface), several times a frame
			Segment::AnchorPresent,
			Segment::DeclaredUi,       // the UI stage (SegmentsOn): the overlay, ending just before the present, HDR output or the capture
			Segment::DeclaredMenuUi,   // the menu UI stage (SegmentsOn): a menu's scene between its displays, ending as the UI stage does
			Segment::AnchorOverlay,    // the overlay's ImGui draw, inside HDRDisplay's present chain
			Segment::AnchorHDROutput,  // HDRDisplay's output composite, after the overlay
			Segment::AnchorCapture,    // the screenshot capture, at the end of HDRDisplay's present chain
		};

		std::uint32_t RankOf(std::uint32_t a_segment)
		{
			for (std::uint32_t i = 0; i < kTimeline.size(); ++i)
				if (static_cast<std::uint32_t>(kTimeline[i]) == a_segment)
					return i;
			return static_cast<std::uint32_t>(kTimeline.size());
		}

		std::uint32_t IndexOf(Anchor a_anchor) { return static_cast<std::uint32_t>(a_anchor); }

		std::uint32_t EpochOfAnchor(Anchor a_anchor) { return RenderGraphRuntime::EpochOf(static_cast<Segment>(SegmentOf(a_anchor))); }

		/**
		 * @brief The declared frame's segments (SegmentsOn). Each is one epoch whose graph holds its steps' passes, in the steps'
		 * order. A step is one hook of the frame (a tap), in the order the engine reaches them:
		 * - an anchor that is wholly the segment's (no client named): every client of it, and the anchor has no epoch of its own;
		 * - one or two named clients of an anchor that keeps its epoch for its other clients (a shared anchor);
		 * - another module's epoch (foreign), whose passes the module hands to the segment (SetForeignPasses).
		 * The last step is the end tap: the stage's one commit. Earlier taps only stage their clients' values: their commits write
		 * into the frame slot's staging run (the frame inputs, GraphHost::EpochStagingRun), never into the ticket's. The end tap
		 * copies the staged values into the slot's copy and submits the stage's one ticket (docs/development/org-native-frame-graph.md,
		 * "The recipe").
		 */
		constexpr std::uint32_t kDeclared = RenderGraph::Frame::kSegments;
		constexpr std::uint32_t kMaxSteps = 16;
		// Each step that reads CS's shared constants uploads them at its own point.
		static_assert(kMaxSteps <= SharedConstants::kUploadPoints, "a stage's steps need an upload point each");
		struct Step
		{
			Anchor anchor = Anchor::Count;  // Count for a foreign or point step
			// The named clients; none: the whole anchor. A name ending in '*' names every client whose id starts with what comes before it.
			std::array<const char*, 4> clients{ nullptr, nullptr, nullptr, nullptr };
			Segment foreign = Segment::Count;  // a foreign step's module epoch
			// With no client named: every client of the anchor while the segment's window is open, and the anchor keeps its own epoch for
			// its runs outside the window (a menu's 3D scene runs the Tonemap anchor after the world's post chain).
			bool shared = false;
			// A CS point that is no anchor and no module epoch (TapPoint): it holds no passes and only ends the segment.
			bool point = false;
			// The step takes taps only until a later step of its stage has tapped this frame; then its anchor's taps go to the stage's next
			// step at the anchor. The menu UI stage's displays before the scene: the first display after the scene's first tap is drawn
			// after the scene.
			bool untilLater = false;
			// The instances of a client with instances (ClientDesc::maxInstances) this step makes and taps: from firstInstance, instanceCount
			// of them (0: every instance from firstInstance). The step's k-th tap this frame is instance firstInstance + k. Two steps at one
			// anchor give the same client disjoint ranges (Scaleform's displays before and after a menu's scene).
			std::uint8_t firstInstance = 0;
			std::uint8_t instanceCount = 0;
		};
		struct DeclaredInfo
		{
			Segment segment;
			std::array<Step, kMaxSteps> steps;
			std::uint32_t stepCount;
			SegmentNames names;
		};
		constexpr std::array<DeclaredInfo, kDeclared> kDeclaredInfo{ {
			// The frame's start (frame sequence census, a world frame): Main::Draw's cubemap render (BSCubeMapCamera::RenderCubemap) runs
			// the ReflectionsPrepass hook once, then each reflection face's sky batch (SkyReflections, once per face drawn: the faces' client
			// has an instance per face). The faces are cleared by the load operations of their sky's first draw; the engine's clears of them
			// are dropped. Its commit is a point just before the next GPU work that follows it in every world frame is submitted: DCLF's
			// shadow view epoch, the shadow clears or a later stage (BeforeGpuWork, and a later stage's first tap). The decal tests come
			// before it and keep their anchor's own epoch (several a frame, each with its own query range).
			// Main::RenderWaterEffects runs after the faces: the water displacement update and the flow copies are the start client's steps
			// at the ImageSpace anchor (ImageSpace's StartSteps.inl), a tap per update and per copy, each with its own instances.
			{ Segment::DeclaredStart,
				{ { { Anchor::ReflectionsPrepass },
					{ Anchor::SkyReflections },
					{ Anchor::ImageSpace, { "cs.engine.imagespace.start", nullptr } },
					{ Anchor::Count, { nullptr, nullptr }, Segment::Count, false, true } } },
				4,
				{ "CS stage: start", "CS stage: start inputs", "FrameSegments::Start / ", "cs.segment.start" } },
			{ Segment::DeclaredPreDepth, { { { Anchor::EarlyPrepass } } }, 1,
				{ "CS segment: pre-depth", "CS segment: pre-depth inputs", "FrameSegments::PreDepth / ", "cs.segment.pre-depth" } },
			// Main::RenderDepth's end to the main pass's sky. The depth copy comes after DCLF's Z-prepass epoch (which holds first
			// person's depth too), and light culling, the Prepass clients and the main pass's sky batch come before DCLF's main opaque
			// epoch: no other GPU work runs in between (the census counts any that does). The sky's batch does not run in every world frame,
			// so the stage ends at a point: just before DCLF's main opaque epoch or a later stage is submitted (BeforeGpuWork, and a later
			// stage's first tap).
			{ Segment::DeclaredPreMain,
				{ { { Anchor::EngineDepthCopy, { "cs.engine.skeleton.depth-copy", nullptr } },
					{ Anchor::DepthDone },
					{ Anchor::EngineDraws, { "cs.engine.skeleton.frame@engine-draws", "cs.shadow-mask.world" } },
					{ Anchor::Count, { nullptr, nullptr }, Segment::LightCulling },
					{ Anchor::Prepass },
					{ Anchor::Sky },
					{ Anchor::Count, { nullptr, nullptr }, Segment::Count, false, true } } },
				7, { "CS segment: pre-main", "CS segment: pre-main inputs", "FrameSegments::PreMain / ", "cs.segment.pre-main" } },
			// Deferred::StartDeferred, after DCLF's main opaque epoch: the deferred passes (with CS's depth copy they host), the composite
			// and the end of the deferred passes; then the rest of Main::RenderWorld (100424 / 107142): the water stencil's clear and the
			// water mask (FinishAccumulating's stencil batch), the reflections, RenderSSAO's two calls with the snow pass or its copy between
			// them, the engine's kMAIN copy, the water, the water targets' clear and the water blend. Its commit is a point just before DCLF's
			// forward epoch is submitted (BeforeGpuWork), the next GPU work after the water blend: every tap has staged by then. The water's
			// draws and the kMAIN copy are gated: a step that does not fire this frame (no water in view) does nothing.
			{ Segment::DeclaredDeferred,
				{ { { Anchor::DeferredPasses },
					{ Anchor::DeferredComposite },
					{ Anchor::PostComposite },
					{ Anchor::EngineDraws, { "cs.engine.skeleton.frame@engine-draws", "cs.water.mask" } },
					{ Anchor::ImageSpace, { "cs.engine.imagespace.world.scopes", nullptr } },
					{ Anchor::EngineCopy, { nullptr, nullptr }, Segment::Count, true },
					{ Anchor::EngineDraws, { "cs.water.surface", nullptr } },
					{ Anchor::ImageSpace, { "cs.engine.skeleton.frame@image-space", "cs.engine.imagespace.world.water" } },
					{ Anchor::Count, { nullptr, nullptr }, Segment::Count, false, true } } },
				9, { "CS stage: deferred+water", "CS stage: deferred+water inputs", "FrameSegments::Deferred / ", "cs.segment.deferred" } },
			// The sun stage: Main::RenderWorld's sun occlusion query (Sun::SubmitOcclusionQuery, SE 0x1403bbf20), after DCLF's forward epoch. It
			// must run on the GPU before the first-person geometry, which would occlude the sun: a stage of its own between DCLF's forward and
			// first-person graphs, committed just before the first-person epoch is submitted (BeforeGpuWork) or at a later stage's first tap.
			// The sun's client has two instances (the world's and first person's query); a stage run's k-th tap is instance k.
			{ Segment::DeclaredSun,
				{ { { .anchor = Anchor::EngineQueries, .clients = { "cs.sky.sun", nullptr, nullptr, nullptr }, .instanceCount = 2 },
					{ Anchor::Count, {}, Segment::Count, false, true } } },
				2, { "CS stage: sun", "CS stage: sun inputs", "FrameSegments::Sun / ", "cs.segment.sun" } },
			// The post stage: ImageSpaceManager::RenderWorldPostProcess (SE 100430, AE 107148). First the first person's sun query
			// (Main::RenderFirstPersonView, after DCLF's first-person epoch), as the sun's client's instance 0 of this stage's run. The volumetric lighting (whole: its hook
			// runs only there), the pre-tonemap composite as CS passes (ChainSteps.inl), the PostProcessing anchor (reserved so its hook
			// always runs), the Upscale anchor (the upscaler's evaluate as a pass recorded at execution, the copy into kMAIN, the depth
			// upscale), RenderEndOfFrameEffects (99023 / 105674) as CS passes split at the Tonemap anchor (its passes replace or follow the
			// tonemap draw, so they sit between the HDR effect's draws and the tonemap's), and its return as the end (a point: no anchor
			// runs there). Nothing else submits GPU work between the first tap and the end (the census counts any that does).
			{ Segment::DeclaredPost,
				{ { { .anchor = Anchor::EngineQueries, .clients = { "cs.sky.sun", nullptr, nullptr, nullptr }, .instanceCount = 2 },
					{ Anchor::VolumetricLighting },
					{ Anchor::ImageSpace, { "cs.engine.imagespace.post.composites", nullptr } },
					{ Anchor::PostProcessing },
					{ Anchor::Upscale },
					{ Anchor::ImageSpace, { "cs.engine.imagespace.post.hdr", nullptr } },
					{ Anchor::Tonemap, { nullptr, nullptr }, Segment::Count, true },
					{ Anchor::ImageSpace, { "cs.engine.imagespace.post.final", "cs.engine.skeleton.frame@post-chain" } },
					{ Anchor::Count, { nullptr, nullptr }, Segment::Count, false, true } } },
				9, { "CS stage: post", "CS stage: post inputs", "FrameSegments::Post / ", "cs.segment.post" } },
			// The UI stage, in the world's and a Bink movie's recipes: the overlay's ImGui draw (its draw list a frame input, recorded in the
			// stage's late batch). It ends at a point: just before the present (Hooks.cpp), or before HDR output or the capture is submitted,
			// which follow the overlay on the GPU and keep their anchors' tickets.
			// First Scaleform's displays: each display segment is a tap and an instance of the Scaleform client (instances 0 to 7, all declared,
			// the draws' counts latched in its run gate); its draw list is a frame input recorded in the late batch. (A Bink frame's clear of the
			// main depth comes before its Begin and has no reader: dropped, Engine/Skeleton.)
			// The Present anchor's clients: a Bink movie's tiles, queued by the movie's tick and drawn from the run's slot in the late batch,
			// and the GPU load diagnostic. The swap chain's Present hook taps it after the displays and before the overlay, as the engine
			// draws them.
			// Last the capture (the Screenshot feature's copy into a readback buffer, CS_ORG_SCREENSHOT): native mode's capture point is just
			// before the stage's commit at the present (Hooks.cpp), after the overlay.
			{ Segment::DeclaredUi,
				{ { { .anchor = Anchor::Interface, .firstInstance = 0, .instanceCount = 8 },
					{ Anchor::Present },
					{ Anchor::Overlay },
					{ Anchor::Capture },
					{ Anchor::Count, {}, Segment::Count, false, true } } },
				5,
				{ "CS stage: UI", "CS stage: UI inputs", "FrameSegments::Ui / ", "cs.segment.ui" } },
			// The menu UI stage: the loading screen, the main menu and a menu over a frozen world (frame sequence census). The engine draws,
			// in this order: the frozen frame's composite (Main::Render; a menu over a frozen world only), the frame's first clears, displays,
			// the shadow map's and the scene's clears, the scene's depth (Drawcall Limit Fix's menu depth batch), the shadow mask, the depth
			// copy and the kMAIN copy, the scene's colour, its composite (UI3DSceneManager::Render's end-of-frame call, ImageSpace's
			// MenuSteps.inl), the main depth's clear again, more displays, the present's clients and the overlay.
			// - Clears are passes at three places, each a client of the frame and shadow clear groups (Engine/Skeleton): the stage's start,
			//   the scene's start and after the composite. The engine clears the main depth three times a frame (for the displays' masks
			//   before the scene, for the scene's depth, for the displays' masks after it) and kMAIN twice; the engine's call stages each
			//   clear into the place it comes at.
			// - The displays before the scene are instances 8 to 15 of the Scaleform client; once a later step tapped, the displays are
			//   instances 0 to 7, after the scene.
			// - The scene's draw lists and the composite's constants are frame inputs, recorded in the stage's late batch.
			// It ends at a point, as the UI stage does.
			{ Segment::DeclaredMenuUi,
				{ { { Anchor::ImageSpace, { "cs.engine.imagespace.frozen" } },
					{ Anchor::EngineFrameClear, { "cs.engine.skeleton.frame@menu-start", "cs.engine.skeleton.shadow@menu-start" }, Segment::Count, true },
					{ .anchor = Anchor::Interface, .untilLater = true, .firstInstance = 8, .instanceCount = 8 },
					{ Anchor::EngineFrameClear, { "cs.engine.skeleton.frame@menu-scene", "cs.engine.skeleton.shadow@menu-scene" }, Segment::Count, true },
					{ Anchor::EngineDraws, { "cs.menu-scenes.menu-depth" } },
					{ Anchor::EngineDraws, { "cs.shadow-mask.menu" } },
					{ Anchor::EngineDepthCopy, { "cs.engine.skeleton.depth-copy" } },
					{ Anchor::EngineCopy, {}, Segment::Count, true },
					{ Anchor::EngineDraws, { "cs.menu-scenes.menu-colour", "cs.menu-scenes.local-map*" } },
					{ Anchor::ImageSpace, { "cs.engine.imagespace.menu" } },
					{ Anchor::EngineFrameClear, { "cs.engine.skeleton.frame@menu-after", "cs.engine.skeleton.shadow@menu-after" }, Segment::Count, true },
					{ .anchor = Anchor::Interface, .firstInstance = 0, .instanceCount = 8 },
					{ Anchor::Present },
					{ Anchor::Overlay },
					{ Anchor::Capture },
					{ Anchor::Count, {}, Segment::Count, false, true } } },
				16,
				{ "CS stage: menu UI", "CS stage: menu UI inputs", "FrameSegments::MenuUi / ", "cs.segment.menu-ui" } },
		} };

		/** @brief The anchors whose every client a declared segment holds, a bit each: they have no epoch of their own. */
		constexpr std::uint32_t WholeMembers(std::uint32_t a_declared)
		{
			std::uint32_t mask = 0;
			for (std::uint32_t k = 0; k < kDeclaredInfo[a_declared].stepCount; ++k) {
				const auto& step = kDeclaredInfo[a_declared].steps[k];
				if (step.anchor != Anchor::Count && !step.clients[0] && !step.shared)
					mask |= 1u << static_cast<std::uint32_t>(step.anchor);
			}
			return mask;
		}

		/** @brief Every anchor a declared segment has a step at, a bit each. */
		constexpr std::uint32_t StepAnchors(std::uint32_t a_declared)
		{
			std::uint32_t mask = 0;
			for (std::uint32_t k = 0; k < kDeclaredInfo[a_declared].stepCount; ++k)
				if (const auto anchor = kDeclaredInfo[a_declared].steps[k].anchor; anchor != Anchor::Count)
					mask |= 1u << static_cast<std::uint32_t>(anchor);
			return mask;
		}

		/** @brief The step of a declared segment at an anchor (the first one), or kMaxSteps. */
		std::uint32_t StepAt(std::uint32_t a_declared, Anchor a_anchor)
		{
			for (std::uint32_t k = 0; k < kDeclaredInfo[a_declared].stepCount; ++k)
				if (kDeclaredInfo[a_declared].steps[k].anchor == a_anchor)
					return k;
			return kMaxSteps;
		}

		bool HasPassesAt(const ClientDesc& a_client, Anchor a_anchor)
		{
			return std::ranges::any_of(a_client.passes, [&](const PassDesc& a_pass) { return a_pass.anchor == a_anchor; });
		}

		/** @brief Whether a step holds a client: every client of a whole anchor, or a named one. */
		bool StepHolds(const Step& a_step, const ClientDesc& a_client)
		{
			if (a_step.anchor == Anchor::Count || !HasPassesAt(a_client, a_step.anchor))
				return false;
			if (!a_step.clients[0])
				return true;
			return std::ranges::any_of(a_step.clients, [&](const char* a_id) {
				if (!a_id)
					return false;
				const std::string_view name(a_id);
				if (!name.empty() && name.back() == '*')
					return a_client.id.starts_with(name.substr(0, name.size() - 1));
				return a_client.id == name;
			});
		}

		/**
		 * @brief Whether a stage's schedules are kept current in frames whose recipe lacks it: the UI stages. A frame-kind switch then finds
		 * the stage's recordings of its description ready, and no display is left out at the switch.
		 */
		constexpr bool KeptAhead(std::uint32_t a_declared)
		{
			return kDeclaredInfo[a_declared].segment == Segment::DeclaredUi || kDeclaredInfo[a_declared].segment == Segment::DeclaredMenuUi;
		}

		/** @brief Whether a step's client a_client has instances this step makes: its range, or every instance from its first. */
		std::pair<std::uint32_t, std::uint32_t> StepInstances(const Step& a_step, const ClientDesc& a_client)
		{
			const std::uint32_t max = std::min<std::uint32_t>(a_client.maxInstances, 255);
			const std::uint32_t first = std::min<std::uint32_t>(a_step.firstInstance, max);
			const std::uint32_t count = a_step.instanceCount ? std::min<std::uint32_t>(a_step.instanceCount, max - first) : max - first;
			return { first, count };
		}

		/** @brief Whether a client of a built snapshot is a_desc's registration: the same desc, or the same id for a matchesById client. */
		bool SameClient(const std::shared_ptr<const ClientDesc>& a_built, const std::shared_ptr<const ClientDesc>& a_desc)
		{
			return a_built == a_desc || (a_built && a_desc && a_built->matchesById && a_built->id == a_desc->id);
		}

		/** @brief Whether a declared segment ends at a point (its last step is no anchor's hook). */
		constexpr bool PointEnded(std::uint32_t a_declared) { return kDeclaredInfo[a_declared].steps[kDeclaredInfo[a_declared].stepCount - 1].point; }

		/** @brief The anchor whose hook ends a declared segment: its last step's. */
		Anchor EndTap(std::uint32_t a_declared) { return kDeclaredInfo[a_declared].steps[kDeclaredInfo[a_declared].stepCount - 1].anchor; }

		std::uint32_t EpochOfDeclared(std::uint32_t a_declared) { return RenderGraphRuntime::EpochOf(kDeclaredInfo[a_declared].segment); }

		/**
		 * @brief What a declared segment's recording is made for: the segment's fields of the frame description. The host's thread
		 * prepares the segment's passes with it (PassPrepareContext::preparationData), and each pass whose client is off in it records
		 * only its barriers (org::RenderGraphPass::SetSkipWhen).
		 */
		struct SegmentHostData final : org::IHostExecutionData
		{
			std::uint32_t segment = 0;
			RenderGraph::Frame::SegmentFields fields;
			const void* TryGet(std::type_index a_type) const noexcept override
			{
				return a_type == std::type_index(typeid(SegmentHostData)) ? this : nullptr;
			}
		};

		/**
		 * @brief One recording of a declared segment, for one description of it (FieldsHash) and one frame slot: the published copy
		 * that slot's runs take. Recording one copy waits for no GPU work, because the slot's last run is four frames old.
		 */
		struct SegmentRecording
		{
			std::uint64_t hash = 0;
			std::uint32_t slot = 0;
			RenderGraph::Frame::SegmentFields fields;
			std::shared_ptr<const org::GraphHost::EpochRecording> recording;  // set once the host's thread recorded every copy
			bool done = false;                                               // the request completed (recorded, failed or dropped)
			bool dropped = false;  // a graph build dropped the request before it was recorded: no failure, it is asked for again
			std::string error;     // why it failed or was dropped
			std::uint64_t lastFrame = 0;                                     // the last frame that asked for it
		};

		/**
		 * @brief A declared segment's recordings: the last few descriptions' (kKeep) for each frame slot, so a field that toggles
		 * records nothing new.
		 */
		struct SegmentRecordings
		{
			static constexpr std::size_t kKeep = 6 * RenderGraphRuntime::kGameFramesInFlight;
			std::mutex mutex;  // the host's thread completes requests under it
			std::vector<std::shared_ptr<SegmentRecording>> entries;
			RenderGraph::Frame::SegmentFields last{};  // the fields of the last description asked for, for the reason a new one logs
			bool any = false;
			// The segment's version generation (SegmentFields::versions): moved when a kept recording binds a version that is no
			// longer current. Render owner.
			std::uint64_t versionGeneration = 0;
		};

		/**
		 * @brief Which passes a CS stage's build hands to ORG (docs/development/org-native-frame-graph.md, "Absent passes"). A pass the
		 * key leaves out is absent from the build: no pass, no barriers, no recording. Per client registration (by its desc, so a client
		 * registered again starts whole): whether its passes are present, and which of its rows. A client the key does not list (one
		 * registered since) is present whole. Instances are never cut: an instance count is a late engine decision, and an empty
		 * instance records no barriers (it skips at execution). Immutable once published.
		 */
		struct BuildKey
		{
			struct Client
			{
				std::shared_ptr<const ClientDesc> desc;
				bool present = true;
				std::array<std::uint64_t, 4> rows{};  // its rows present (bits of rows its passes carry)
			};
			bool all = true;  // every pass: a build before the stage's first key (setup), or with absence off
			std::vector<Client> clients;
			std::uint32_t foreign = 0;  // the module steps present (a bit per step)
			std::uint64_t serial = 0;   // keys are numbered as they are made
			std::uint64_t frame = 0;    // the frame whose Begin made it

			const Client* Find(const ClientDesc* a_desc) const
			{
				for (const auto& client : clients)
					if (client.desc.get() == a_desc)
						return &client;
				return nullptr;
			}
		};

		/** @brief Whether a key (null: every pass) has a pass present. a_row: the pass's row, -1 for none. */
		bool KeyHas(const BuildKey* a_key, const ClientDesc& a_client, std::int32_t a_row)
		{
			if (!a_key || a_key->all)
				return true;
			const auto* client = a_key->Find(&a_client);
			if (!client)
				return true;
			if (!client->present)
				return false;
			return a_row < 0 || a_row >= 256 || (client->rows[static_cast<std::size_t>(a_row) / 64] >> (static_cast<std::uint32_t>(a_row) % 64) & 1u) != 0;
		}

		/** @brief The rows a client's passes carry, a bit each. */
		std::array<std::uint64_t, 4> RowsOf(const ClientDesc& a_client)
		{
			std::array<std::uint64_t, 4> rows{};
			for (const auto& pass : a_client.passes)
				if (pass.row >= 0 && pass.row < 256)
					rows[static_cast<std::size_t>(pass.row) / 64] |= 1ull << (static_cast<std::uint32_t>(pass.row) % 64);
			return rows;
		}

		/** @brief A client's part of a key (null: every pass): present or not, and its rows present. */
		std::pair<bool, std::array<std::uint64_t, 4>> KeyPart(const BuildKey* a_key, const ClientDesc& a_client)
		{
			const auto own = RowsOf(a_client);
			if (!a_key || a_key->all)
				return { true, own };
			const auto* client = a_key->Find(&a_client);
			if (!client)
				return { true, own };
			if (!client->present)
				return { false, {} };
			std::array<std::uint64_t, 4> rows{};
			for (std::size_t w = 0; w < rows.size(); ++w)
				rows[w] = own[w] & client->rows[w];
			return { true, rows };
		}

		/** @brief The module step bits a key has present, of a_steps (a bit per module step; null key: all of them). */
		std::uint32_t KeyForeign(const BuildKey* a_key, std::uint32_t a_steps)
		{
			return !a_key || a_key->all ? a_steps : a_key->foreign & a_steps;
		}

		/** @brief What one key has that the other does not, over a_clients and a_steps module steps, as text; empty when equal. */
		std::string DiffKeys(const BuildKey* a_before, const BuildKey* a_after, const std::vector<std::shared_ptr<const ClientDesc>>& a_clients, std::uint32_t a_steps)
		{
			std::string added, dropped;
			const auto note = [](std::string& a_text, std::string_view a_what) { a_text += fmt::format("{}{}", a_text.empty() ? "" : ", ", a_what); };
			for (const auto& client : a_clients) {
				const auto [wasOn, wasRows] = KeyPart(a_before, *client);
				const auto [isOn, isRows] = KeyPart(a_after, *client);
				if (wasOn != isOn) {
					note(isOn ? added : dropped, client->id);
					continue;
				}
				std::uint32_t plus = 0, minus = 0;
				for (std::size_t w = 0; w < 4; ++w) {
					plus += static_cast<std::uint32_t>(std::popcount(isRows[w] & ~wasRows[w]));
					minus += static_cast<std::uint32_t>(std::popcount(wasRows[w] & ~isRows[w]));
				}
				if (plus)
					note(added, fmt::format("{} rows of {}", plus, client->id));
				if (minus)
					note(dropped, fmt::format("{} rows of {}", minus, client->id));
			}
			const auto plusSteps = KeyForeign(a_after, a_steps) & ~KeyForeign(a_before, a_steps);
			const auto minusSteps = KeyForeign(a_before, a_steps) & ~KeyForeign(a_after, a_steps);
			if (plusSteps)
				note(added, fmt::format("module steps {:#x}", plusSteps));
			if (minusSteps)
				note(dropped, fmt::format("module steps {:#x}", minusSteps));
			std::string text;
			if (!added.empty())
				text += fmt::format("present now: {}", added);
			if (!dropped.empty())
				text += fmt::format("{}absent now: {}", text.empty() ? "" : "; ", dropped);
			return text;
		}

		/** @brief Whether every pass a_inner has present is present in a_outer too, over a_clients and a_steps module steps. */
		bool KeyCovers(const BuildKey* a_outer, const BuildKey* a_inner, const std::vector<std::shared_ptr<const ClientDesc>>& a_clients, std::uint32_t a_steps)
		{
			for (const auto& client : a_clients) {
				const auto [outerOn, outerRows] = KeyPart(a_outer, *client);
				const auto [innerOn, innerRows] = KeyPart(a_inner, *client);
				if (innerOn && !outerOn)
					return false;
				for (std::size_t w = 0; w < 4; ++w)
					if (innerRows[w] & ~outerRows[w])
						return false;
			}
			return (KeyForeign(a_inner, a_steps) & ~KeyForeign(a_outer, a_steps)) == 0;
		}

		/**
		 * @brief Whether a key has some passes present only because it does not know their client: a build before the stage's first key, or
		 * one made before a client registered.
		 */
		bool KeyProvisional(const BuildKey* a_key, const std::vector<std::shared_ptr<const ClientDesc>>& a_clients)
		{
			if (!a_key || a_key->all)
				return true;
			return std::ranges::any_of(a_clients, [&](const auto& a_client) { return !a_key->Find(a_client.get()); });
		}

		/**
		 * @brief A stage's use of its passes, for its next key (render owner): the stage's own clock, which runs only in frames whose recipe
		 * has the stage, and when each client registration, each of its rows and each module step was last wanted, on that clock.
		 */
		struct KeyUse
		{
			struct Client
			{
				std::shared_ptr<const ClientDesc> desc;
				double used = -1e30;
				std::array<double, 256> rows;
				Client() { rows.fill(-1e30); }
			};
			double clock = 0.0;
			std::chrono::steady_clock::time_point last{};
			std::vector<Client> clients;
			std::array<double, 32> foreign;
			std::uint64_t requestedSerial = 0;  // the last key a build was asked for with, and the frame it was asked at
			std::uint64_t requestedFrame = 0;
			std::uint64_t switchedSerial = 0;  // the last requested key whose build switched in (its lag is logged once)
			KeyUse() { foreign.fill(-1e30); }
		};

		/** @brief D3D11 textures the imports let go of on any thread, released on the render thread (ServiceRetiredTextures). */
		struct RetiredTextures
		{
			std::mutex mutex;
			std::vector<winrt::com_ptr<ID3D11Texture2D>> textures;
			std::atomic<bool> any{ false };
		};

		RetiredTextures& Retired()
		{
			// Never destroyed: entries let go of their textures during static destruction too.
			static auto* retired = new RetiredTextures;
			return *retired;
		}

		/**
		 * @brief Render thread: the D3D11 textures let go of since the last call go once D3D11's last use is done. A loose slot's last
		 * version may be dropped on any thread (a graph build, the host), and D3D11 releases belong on the render thread.
		 */
		void ServiceRetiredTextures()
		{
			auto& retired = Retired();
			if (!retired.any.load(std::memory_order_acquire))
				return;
			auto textures = std::make_shared<std::vector<winrt::com_ptr<ID3D11Texture2D>>>();
			{
				const std::lock_guard lock(retired.mutex);
				textures->swap(retired.textures);
				retired.any.store(false, std::memory_order_release);
			}
			RenderGraphRuntime::Get().ReleaseAfterD3D11(std::move(textures));
		}

		/**
		 * @brief An image's one graph resource: its slot. Two kinds:
		 * - an id import (State::imports): the slot lasts as long as an id names it; a new D3D11 texture of the same shape class is
		 *   the slot's next version. Several ids of one image share one entry.
		 * - a loose one (State::loose, ShareImage before any id import of the image): the slot lives while its holders hold it. Each
		 *   of its versions keeps this entry, and with it the D3D11 texture, as its owner; the state only watches it.
		 */
		struct EngineImport
		{
			winrt::com_ptr<ID3D11Texture2D> texture;  // the current version's D3D11 texture
			std::shared_ptr<org::VersionedTexture> slot;     // an id import's
			std::weak_ptr<org::VersionedTexture> looseSlot;  // a loose one's
			std::string id;                                  // the id (or the loose holder's name) it was made under, for the logs
			std::uint64_t image = 0;                         // the current version's VkImage (ID3D12Resource on D3D12)
			DxvkOrgInteropImageInfo info{};                  // the current version's image, to import it again with more views
			org::TextureDescription views;                   // the views the current version has: what a new importer's are compared with

			std::shared_ptr<org::VersionedTexture> Slot() const { return slot ? slot : looseSlot.lock(); }

			EngineImport() = default;
			EngineImport(const EngineImport&) = default;
			EngineImport& operator=(const EngineImport&) = default;

			/** @brief The D3D11 texture goes to the render thread: this may be the last version's owner, dropped on any thread. */
			~EngineImport()
			{
				if (!texture)
					return;
				auto& retired = Retired();
				const std::lock_guard lock(retired.mutex);
				retired.textures.push_back(std::move(texture));
				retired.any.store(true, std::memory_order_release);
			}
		};

		/** @brief One pass of the build: its client (which owns its desc) and the desc. */
		struct ScheduledPass
		{
			std::shared_ptr<const ClientDesc> client;
			const PassDesc* pass = nullptr;
		};

		/** @brief What every graph build makes the anchors' passes and resources from. Immutable once published. */
		struct Snapshot
		{
			std::uint64_t serial = 0;                                // State::serial when it was published
			std::vector<std::shared_ptr<const ClientDesc>> clients;  // registration order, the ones in the graphs
			std::vector<ScheduledPass> passes;                       // by anchor, then order key, then registration
			std::vector<std::pair<std::string, std::shared_ptr<org::VersionedTexture>>> imports;  // each id's slot
			// Each declared segment's clients, in registration order (SegmentClients). Made once, at Publish.
			std::array<std::vector<std::shared_ptr<const ClientDesc>>, kDeclared> segmentClients;
		};

		/** @brief A registered client and what the anchors keep about it. Render thread. */
		struct Client
		{
			std::shared_ptr<const ClientDesc> desc;
			// The snapshot that first held its passes and resources as they are now (Register, Refresh, a resize): a graph built
			// from that one or a later one runs them.
			std::uint64_t serial = 0;
			// Per pass: the snapshot a pending one-shot pass was armed in; 0 once it ran (always 0 for other passes).
			std::vector<std::uint64_t> armed;
			// The declared segments that hold it (a bit per segment), for the description heldOf. A tap asks this, not the steps'
			// client ids by string.
			const ClientDesc* heldOf = nullptr;
			std::uint64_t held = 0;
		};

		/**
		 * @brief The run gates of a registration that left the graphs (unregistered, replaced or dropped). The anchors' graphs built
		 * before snapshot serial still hold its passes, which replay whenever another client runs the anchor: every such run closes
		 * these gates, so the old passes do no work and never read latch slots nobody wrote for that run.
		 */
		struct RetiredGates
		{
			std::vector<std::shared_ptr<RenderGraph::RunGate>> gates;
			std::uint32_t mask = 0;    // the anchors its passes were at
			std::uint64_t serial = 0;  // the first snapshot without them
		};

		/** @brief The anchors' state. Render thread, but the snapshot and the built serials, which a build writes and reads. */
		struct State
		{
			std::array<bool, kAnchors> reserved{};
			bool frozen = false;  // AddToFrame ran: the reservations are the frame's epochs
			org::GraphHost* host = nullptr;
			std::vector<Client> clients;
			std::map<std::string, std::shared_ptr<EngineImport>, std::less<>> imports;  // by graph id; the ids of one image share one
			std::vector<std::weak_ptr<EngineImport>> loose;                             // ShareImage's slots no id names
			std::uint64_t serial = 0;                                  // bumped by every Publish
			std::uint64_t resizeGeneration = 0;                        // GraphResources' last seen
			std::function<void()> lateHook;
			std::vector<RetiredGates> retired;
			// Per anchor: its submitted runs since the host's seal runsSeal (the last Present), and the most it ever had in one frame
			// (diagnostics: LogRunStats).
			std::array<std::uint32_t, kAnchors> runsThisFrame{};
			std::array<std::uint64_t, kAnchors> runsSeal{};
			std::array<std::atomic<std::uint32_t>, kAnchors> peakRuns{};
			std::atomic<std::shared_ptr<const Snapshot>> snapshot;
			std::array<std::atomic<std::uint64_t>, kAnchors> built{};  // the snapshot each anchor's graph was last built from
			// The snapshot each anchor's last build was made from, and the one before it. An epoch failure drops the clients of the
			// failed build (OnEpochFailed); a failed build that kept the old graph puts the one before back.
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kAnchors> builtSnapshots{};
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kAnchors> previousSnapshots{};
			// Declared segments: whether each has an epoch (AddToFrame), the anchors they hold (which then have no epoch of their own),
			// each segment graph's snapshots as for the anchors above, and each segment's recordings.
			std::array<bool, kDeclared> declaredEpochs{};
			std::uint32_t declaredMembers = 0;
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kDeclared> declaredSnapshots{};
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kDeclared> declaredPrevious{};
			std::array<SegmentRecordings, kDeclared> declaredRecordings;
			// Each segment graph's passes recorded ahead, from its last attached build: their revisions go into its description.
			std::array<std::atomic<std::shared_ptr<std::vector<std::weak_ptr<org::RenderPass>>>>, kDeclared> declaredPasses{};
			// A stage's staged build (GraphHost::StageEpochGraph, BuildManager::Desc::staged): the snapshot it was built from and its passes
			// recorded ahead, set when it is staged. BeginFrame records it for every frame slot and switches it in whole.
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kDeclared> stagedSnapshots{};
			std::array<std::atomic<std::shared_ptr<std::vector<std::weak_ptr<org::RenderPass>>>>, kDeclared> stagedPasses{};
			// Absent passes (docs/development/org-native-frame-graph.md, "Absent passes"): per stage, the key the next build takes (null:
			// every pass), the attached build's and the staged build's, and the stage's use of its passes. A build reads nextKeys when it
			// starts. Render owner, but the keys, which a build reads.
			std::array<std::atomic<std::shared_ptr<const BuildKey>>, kDeclared> nextKeys{};
			std::array<std::atomic<std::shared_ptr<const BuildKey>>, kDeclared> declaredKeys{};
			std::array<std::atomic<std::shared_ptr<const BuildKey>>, kDeclared> stagedKeys{};
			std::array<KeyUse, kDeclared> keyUse;
			std::uint64_t keySerial = 0;
			bool describingKey = false;  // BeginFrame describes a stage's key: every variant a frame may pick, and the passes always present
			// While a client's rows are described for a frame: the build's key the frame is described against, and the client (KeyHasRow).
			const BuildKey* describingAgainst = nullptr;
			const ClientDesc* describingClient = nullptr;
			RenderGraph::Frame::Kind keyKind = RenderGraph::Frame::Kind::Count;  // the last frame's kind, for the key's kind switch
			// Each stage's frame inputs (docs/development/org-native-frame-graph.md, "The recipe"): the frame its taps staged values in
			// and have not committed yet (0: none), the frame slot's staging run they wrote (GraphHost::EpochStagingRun, set at Begin),
			// and the clients whose executed callbacks the commit calls, with each tap's point. Render owner.
			std::array<std::uint64_t, kDeclared> stagedFrame{};
			std::array<org::RunRef, kDeclared> stagingRun{};
			std::array<std::vector<std::pair<std::shared_ptr<const ClientDesc>, Point>>, kDeclared> stagedExecuted{};
			// The schedule (docs/development/org-native-frame-graph.md, "Recipe plan, stage 1"): per segment and frame slot, the
			// description the slot's frames run on, its recording (made on the host's thread for the attached build) and the segment
			// graph's snapshot it was made on. A frame takes its slot's schedule at Begin and keeps it to its end. Render owner.
			struct Scheduled
			{
				RenderGraph::Frame::SegmentFields fields;
				std::shared_ptr<const org::GraphHost::EpochRecording> recording;
				std::shared_ptr<const Snapshot> built;
			};
			std::array<std::array<Scheduled, RenderGraphRuntime::kGameFramesInFlight>, kDeclared> schedule{};
			// This frame's: the schedule's recording and snapshot (null when the segment is absent), and the frame's own description of
			// the segment, which the taps compare a lagging schedule with. Render owner.
			std::array<std::shared_ptr<const org::GraphHost::EpochRecording>, kDeclared> frameRecording{};
			std::array<std::shared_ptr<const Snapshot>, kDeclared> frameBuilt{};
			std::array<RenderGraph::Frame::SegmentFields, kDeclared> frameWanted{};
			// The description a slot last began to wait for, per segment, and the frame it did: a switch's lag counts from it.
			std::array<std::uint64_t, kDeclared> waitHash{};
			std::array<std::uint64_t, kDeclared> waitFrame{};
			// The frame a stage's staged build was first seen at Begin (0: none): its switch's lag counts from it.
			std::array<std::uint64_t, kDeclared> stagedSince{};
			// Whether this frame's recording bound the current versions at Begin: a version published after Begin is a mismatch, one
			// published before it lags until the new recording is ready.
			std::array<bool, kDeclared> frameVersionsCurrent{};
			bool inBegin = false;  // BeginFrame is choosing the schedules: a recording asked for outside it is a mid-frame re-record
			std::array<std::uint64_t, kDeclared> endedFrame{};
			// Each segment's taps of each step in its held run so far: the next tap of a step is that instance (TapInstance). Reset
			// when the frame's first tap takes the ticket.
			std::array<std::array<std::uint32_t, kMaxSteps>, kDeclared> stepTaps{};
			std::uint32_t tapInstance = 0;  // the instance the commits running now write (TapInstance)
			std::atomic<bool> stagesOpen{ false };  // a stage holds staged values not committed yet: BeforeGpuWork's check without the mutex
			// GPU work submitted between a stage's first tap and its commit (BeforeGpuWork), by why, since the last report. It runs ahead of
			// the stage's work on the GPU.
			std::vector<std::pair<const char*, std::uint64_t>> insideHeld;
			std::uint64_t insideHeldLines = 0;
			// The epoch an anchor's run counted there already (Run, with the anchor's name). Its submission calls BeforeGpuWork again
			// for the same epoch, which then counts nothing. UINT32_MAX: none.
			std::uint32_t insideHeldCounted = UINT32_MAX;
		};

		State& Anchors()
		{
			static State s_state;
			return s_state;
		}

		/**
		 * @brief With CS_ORG_LOADING_THREAD=1, the anchors' state is also under this mutex: the render owner's runs and claims, and every
		 * call that changes the clients or imports (a feature's Register on the main thread during a load, while the loading thread
		 * runs). Recursive: a run's commit, executed and onFailed callbacks may register. Nothing under it takes Renderer::Lock, and
		 * the host's thread never takes it (the builds read the published snapshot). Without the switch it is not taken.
		 */
		std::unique_lock<std::recursive_mutex> Guard()
		{
			if (!RenderGraph::Engine::LoadingScreen::Requested())
				return {};
			static std::recursive_mutex s_mutex;
			return std::unique_lock(s_mutex);
		}

		bool HasPasses(const ClientDesc& a_client, Anchor a_anchor)
		{
			return std::ranges::any_of(a_client.passes, [&](const PassDesc& a_pass) { return a_pass.anchor == a_anchor; });
		}

		/** @brief Whether the client is in the graphs: one that reads the shared constants waits until they exist. */
		bool InGraphs(const ClientDesc& a_client)
		{
			return !a_client.sharedConstants || SharedConstants::Ready();
		}

		/** @brief Whether a client with passes at the anchor reads the shared constants: the anchor's epoch then uploads them. */
		bool UsesSharedConstants(const std::vector<std::shared_ptr<const ClientDesc>>& a_clients, Anchor a_anchor)
		{
			return std::ranges::any_of(a_clients, [&](const auto& a_client) { return a_client->sharedConstants && HasPasses(*a_client, a_anchor); });
		}

		/** @brief Whether the client has a one-shot pass at the anchor that has not run yet. */
		bool HasPendingOnce(const Client& a_client, Anchor a_anchor)
		{
			for (std::size_t i = 0; i < a_client.desc->passes.size(); ++i)
				if (a_client.armed[i] && a_client.desc->passes[i].anchor == a_anchor)
					return true;
			return false;
		}

		Client* FindClient(State& a_state, std::string_view a_id)
		{
			const auto found = std::ranges::find_if(a_state.clients, [&](const Client& a_client) { return a_client.desc->id == a_id; });
			return found != a_state.clients.end() ? &*found : nullptr;
		}

		/** @brief The anchor of an anchor epoch's graph, or Count. */
		Anchor AnchorOfEpoch(std::uint32_t a_epoch)
		{
			for (std::uint32_t i = 0; i < kAnchors; ++i)
				if (a_epoch == EpochOfAnchor(static_cast<Anchor>(i)))
					return static_cast<Anchor>(i);
			return Anchor::Count;
		}

		/** @brief The declared segment of a segment epoch's graph, or kDeclared. */
		std::uint32_t DeclaredOfEpoch(std::uint32_t a_epoch)
		{
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if (Anchors().declaredEpochs[d] && a_epoch == EpochOfDeclared(d))
					return d;
			return kDeclared;
		}

		/**
		 * @brief The declared segment whose epoch holds anchor a_index's passes, or kDeclared when the anchor has its own epoch. Two stages may
		 * hold an anchor wholly (the UI and menu UI stages: Interface, Present, Overlay): the one in this frame's recipe, else the first.
		 */
		std::uint32_t DeclaredOfAnchor(const State& a_state, std::uint32_t a_index)
		{
			if (!(a_state.declaredMembers >> a_index & 1u))
				return kDeclared;
			const auto& frame = RenderGraph::Frame::Driver::Current();
			std::uint32_t first = kDeclared;
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if (a_state.declaredEpochs[d] && (WholeMembers(d) >> a_index & 1u)) {
					if (RenderGraph::Frame::Recipes::KindHas(frame.kind, frame.capture, static_cast<RenderGraph::Frame::SegmentId>(d)))
						return d;
					if (first == kDeclared)
						first = d;
				}
			return first;
		}

		/** @brief The snapshot the anchor's own graph was built from. Its own graph always holds its passes. */
		std::shared_ptr<const Snapshot> OwnSnapshot(const State& a_state, std::uint32_t a_index)
		{
			return a_state.builtSnapshots[a_index].load(std::memory_order_acquire);
		}

		/** @brief OwnSnapshot's serial: a client registered (or refreshed) at that serial or before is in the anchor's built graph. */
		std::uint64_t BuiltSerial(const State& a_state, std::uint32_t a_index)
		{
			const auto built = OwnSnapshot(a_state, a_index);
			return built ? built->serial : 0;
		}

		/** @brief The epochs whose graphs hold (or will hold) the passes of the anchors in a_mask: each one's own, and the stages that hold them. */
		std::vector<std::uint32_t> EpochsOf(const State& a_state, std::uint32_t a_mask)
		{
			std::vector<std::uint32_t> epochs;
			for (std::uint32_t i = 0; i < kAnchors; ++i) {
				if (!(a_mask >> i & 1u) || !a_state.reserved[i])
					continue;
				// A declared segment's member has no epoch of its own: its passes are in the segments' that hold it.
				if (a_state.declaredMembers >> i & 1u) {
					for (std::uint32_t d = 0; d < kDeclared; ++d)
						if (a_state.declaredEpochs[d] && (StepAnchors(d) >> i & 1u) && std::ranges::find(epochs, EpochOfDeclared(d)) == epochs.end())
							epochs.push_back(EpochOfDeclared(d));
					continue;
				}
				epochs.push_back(EpochOfAnchor(static_cast<Anchor>(i)));
				// A shared anchor whose named clients a segment holds: the segment's graph holds their passes too.
				for (std::uint32_t d = 0; d < kDeclared; ++d)
					if (a_state.declaredEpochs[d] && (StepAnchors(d) >> i & 1u) && std::ranges::find(epochs, EpochOfDeclared(d)) == epochs.end())
						epochs.push_back(EpochOfDeclared(d));
			}
			return epochs;
		}

		bool SegmentHolds(std::uint32_t a_declared, const ClientDesc& a_client);

		/** @brief Publishes the clients and imports as they are now, for the next builds. */
		void Publish(State& a_state)
		{
			auto snapshot = std::make_shared<Snapshot>();
			snapshot->serial = ++a_state.serial;
			for (const auto& client : a_state.clients) {
				if (!InGraphs(*client.desc))
					continue;
				snapshot->clients.push_back(client.desc);
				for (std::size_t i = 0; i < client.desc->passes.size(); ++i) {
					const auto& pass = client.desc->passes[i];
					// A one-shot pass that ran is left out until it is armed again.
					if (!pass.once || client.armed[i])
						snapshot->passes.push_back({ client.desc, &pass });
				}
			}
			// Stable: equal keys keep the registration order, and each client's own.
			std::ranges::stable_sort(snapshot->passes, [](const ScheduledPass& a, const ScheduledPass& b) {
				if (a.pass->anchor != b.pass->anchor)
					return a.pass->anchor < b.pass->anchor;
				return a.pass->order < b.pass->order;
			});
			for (const auto& [id, import] : a_state.imports)
				snapshot->imports.emplace_back(id, import->slot);
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				for (const auto& client : snapshot->clients)
					if (SegmentHolds(d, *client))
						snapshot->segmentClients[d].push_back(client);
			a_state.snapshot.store(std::move(snapshot), std::memory_order_release);
		}

		/** @brief The client's passes and resources, and its pending one-shot passes, as of the snapshot just published. */
		void MarkCurrent(const State& a_state, Client& a_client)
		{
			a_client.serial = a_state.serial;
			for (auto& armed : a_client.armed)
				if (armed)
					armed = a_state.serial;
		}

		/** @brief The anchors' epochs built again at their next execution (the others keep their graphs); a_reason is logged with the build. */
		void Rebuild(State& a_state, std::uint32_t a_anchorMask, std::string_view a_reason)
		{
			if (!a_state.host)
				return;
			const auto reason = fmt::format("frame anchors: {}", a_reason);
			for (const auto epoch : EpochsOf(a_state, a_anchorMask))
				RenderGraph::Builds::Request(epoch, reason);
		}

		/**
		 * @brief Rebuild for a resource identity change no version stands for (a replaced import, a resized twin that is not
		 * versioned): the anchors' epochs, through GraphResources::RequestStructuralRebuild.
		 */
		void StructuralRebuild(State& a_state, std::uint32_t a_anchorMask, std::string_view a_reason)
		{
			if (!a_state.host)
				return;
			const auto epochs = EpochsOf(a_state, a_anchorMask);
			if (!epochs.empty())
				GraphResources::RequestStructuralRebuild(fmt::format("frame anchors: {}", a_reason), epochs);
		}

		std::uint32_t AnchorMask(const ClientDesc& a_client, bool a_onceOnly = false)
		{
			std::uint32_t mask = 0;
			for (const auto& pass : a_client.passes)
				if (IndexOf(pass.anchor) < kAnchors && (!a_onceOnly || pass.once))
					mask |= 1u << IndexOf(pass.anchor);
			return mask;
		}

		/**
		 * @brief After Publish: a_client's registration left the graphs. Its gates (but a_kept, which the registration replacing it
		 * still has and closes as its own) are closed at every run of its anchors until each is built from this snapshot or a later
		 * one (RetiredGates).
		 */
		void RetireGates(State& a_state, const ClientDesc& a_client, const std::vector<std::shared_ptr<RenderGraph::RunGate>>& a_kept = {})
		{
			std::vector<std::shared_ptr<RenderGraph::RunGate>> gates;
			for (const auto& gate : a_client.gates)
				if (gate && std::ranges::find(a_kept, gate) == a_kept.end())
					gates.push_back(gate);
			if (!gates.empty())
				a_state.retired.push_back({ std::move(gates), AnchorMask(a_client), a_state.serial });
		}

		/** @brief Retired gates whose anchors' graphs were all built without their passes are let go. */
		void PruneRetired(State& a_state)
		{
			const auto builtFrom = [](const std::shared_ptr<const Snapshot>& a_built) { return a_built ? a_built->serial : 0; };
			std::erase_if(a_state.retired, [&](const RetiredGates& a_retired) {
				for (std::uint32_t i = 0; i < kAnchors; ++i) {
					if (!(a_retired.mask >> i & 1u) || !a_state.reserved[i])
						continue;
					if (a_state.built[i].load(std::memory_order_acquire) < a_retired.serial)
						return false;
					// A declared segment's graph holds its members' passes.
					for (std::uint32_t d = 0; d < kDeclared; ++d)
						if (a_state.declaredEpochs[d] && (StepAnchors(d) >> i & 1u) &&
							builtFrom(a_state.declaredSnapshots[d].load(std::memory_order_acquire)) < a_retired.serial)
							return false;
				}
				return true;
			});
		}

		/** @brief The anchor's submitted runs since the host's last seal (SealPresented): the count starts again at each seal. */
		std::uint32_t& RunsSinceSeal(State& a_state, std::uint32_t a_index)
		{
			const std::uint64_t seals = a_state.host->Seals();
			if (a_state.runsSeal[a_index] != seals) {
				a_state.runsSeal[a_index] = seals;
				a_state.runsThisFrame[a_index] = 0;
			}
			return a_state.runsThisFrame[a_index];
		}

		/** @brief Whether the client waits for one of its anchors' graphs to be built with its registration. */
		bool WaitsToJoin(const State& a_state, const Client& a_client)
		{
			if (!InGraphs(*a_client.desc))
				return false;
			const std::uint32_t mask = AnchorMask(*a_client.desc);
			for (std::uint32_t i = 0; i < kAnchors; ++i)
				if ((mask >> i & 1u) && a_state.reserved[i] && a_client.serial > BuiltSerial(a_state, i))
					return true;
			return false;
		}

		/**
		 * @brief The clients with passes at the anchor in a failed build (a_failed: the snapshot it was built from), still registered as
		 * they were then, are dropped and return to D3D11; the anchor, and their other anchors, are built again without them. A client
		 * registered again since is not in that build, and its rebuild is already asked for. Render thread.
		 */
		void DropFailedClients(std::uint32_t a_anchors, const char* a_name, const std::shared_ptr<const Snapshot>& a_failed, const std::string& a_error)
		{
			// Native mode has no D3D11 path to drop clients to: their work would reach the frame with no pass and stop it later as a
			// mismatch that hides this error. Stop here with the build's own error (the mismatch rule's switch).
			if (RenderGraph::Engine::Native::Active() && RenderGraph::Frame::Driver::MismatchFatal()) {
				const auto message = fmt::format("Community Shaders native mode: the {} graph failed to build: {}. Development builds stop here "
												 "(CS_ORG_MISMATCH_FATAL=0 drops its clients instead).",
					a_name, a_error);
				logger::critical("[ORG] frame anchors: {}", message);
				spdlog::default_logger()->flush();
				stl::report_and_fail(message);
			}
			auto lock = Guard();
			auto& state = Anchors();
			std::vector<std::shared_ptr<const ClientDesc>> dropped;
			std::uint32_t mask = a_anchors;
			if (a_failed)
				for (const auto& client : a_failed->clients) {
					if (!(AnchorMask(*client) & a_anchors))
						continue;
					const auto found = std::ranges::find_if(state.clients, [&](const Client& a_client) { return a_client.desc == client; });
					if (found == state.clients.end())
						continue;
					mask |= AnchorMask(*client);
					dropped.push_back(client);
					state.clients.erase(found);
				}
			if (dropped.empty()) {
				logger::error(
					"[ORG] frame anchors: the {} epoch failed and none of its build's clients is still registered; it runs nothing new until a "
					"client registers or changes. Why: {}",
					a_name, a_error);
				return;
			}
			// The anchor, and the dropped clients' other anchors, built again without them.
			Publish(state);
			for (const auto& client : dropped)
				RetireGates(state, *client);
			Rebuild(state, mask, "clients dropped after a failed build");
			std::string ids;
			for (const auto& client : dropped)
				ids += fmt::format("{}{}", ids.empty() ? "" : ", ", client->id);
			logger::error(
				"[ORG] frame anchors: the {} epoch failed; its clients {} are dropped and return to D3D11, and the anchor is built again "
				"without them. Why: {}",
				a_name, ids, a_error);
			for (const auto& client : dropped)
				if (client->onFailed)
					client->onFailed();
		}

		/** @brief Whether a client with passes at one of the anchors in a_mask reads the shared constants. */
		bool UsesSharedConstantsIn(const std::vector<std::shared_ptr<const ClientDesc>>& a_clients, std::uint32_t a_mask)
		{
			return std::ranges::any_of(a_clients, [&](const auto& a_client) { return a_client->sharedConstants && (AnchorMask(*a_client) & a_mask); });
		}

		/**
		 * @brief The anchors' passes in each graph: an anchor epoch's graph takes its anchor's passes, a stage's graph its steps' passes;
		 * each registers the clients' resources and the engine imports. Every other epoch's graph
		 * gets nothing from it.
		 */
		/** @brief Whether a declared segment holds a client: one of its steps does. */
		bool SegmentHolds(std::uint32_t a_declared, const ClientDesc& a_client)
		{
			for (std::uint32_t k = 0; k < kDeclaredInfo[a_declared].stepCount; ++k)
				if (StepHolds(kDeclaredInfo[a_declared].steps[k], a_client))
					return true;
			return false;
		}

		/** @brief Whether a declared segment holds a pass: a step at its anchor holds its client. */
		bool SegmentHoldsPass(std::uint32_t a_declared, const ClientDesc& a_client, const PassDesc& a_pass)
		{
			for (std::uint32_t k = 0; k < kDeclaredInfo[a_declared].stepCount; ++k) {
				const auto& step = kDeclaredInfo[a_declared].steps[k];
				if (step.anchor == a_pass.anchor && StepHolds(step, a_client))
					return true;
			}
			return false;
		}

		/** @brief A declared segment's clients in a snapshot, in registration order: the order of the description's clientsOn bits. */
		const std::vector<std::shared_ptr<const ClientDesc>>& SegmentClients(const Snapshot& a_snapshot, std::uint32_t a_declared)
		{
			return a_snapshot.segmentClients[a_declared];
		}

		/** @brief Whether a declared segment holds a registered client (SegmentHolds, kept per description). Render thread. */
		bool ClientHolds(Client& a_client, std::uint32_t a_declared)
		{
			if (a_client.heldOf != a_client.desc.get()) {
				a_client.held = 0;
				for (std::uint32_t d = 0; d < kDeclared; ++d)
					if (SegmentHolds(d, *a_client.desc))
						a_client.held |= std::uint64_t{ 1 } << d;
				a_client.heldOf = a_client.desc.get();
			}
			return (a_client.held >> a_declared & 1u) != 0;
		}

		/** @brief Whether a step's clients in a snapshot read CS's shared constants: the segment uploads them for that step. */
		bool StepUsesSharedConstants(const Snapshot& a_snapshot, std::uint32_t a_declared, std::uint32_t a_step)
		{
			const auto& step = kDeclaredInfo[a_declared].steps[a_step];
			return std::ranges::any_of(a_snapshot.clients, [&](const auto& a_client) { return a_client->sharedConstants && StepHolds(step, *a_client); });
		}

		/** @brief Whether any step of a declared segment reads CS's shared constants. */
		bool SegmentUsesSharedConstants(const Snapshot& a_snapshot, std::uint32_t a_declared)
		{
			for (std::uint32_t k = 0; k < kDeclaredInfo[a_declared].stepCount; ++k)
				if (StepUsesSharedConstants(a_snapshot, a_declared, k))
					return true;
			return false;
		}

		/** @brief A module epoch's passes a declared segment holds (SetForeignPasses). */
		struct ForeignPasses
		{
			std::function<void(std::uint32_t, void*)> gather;
			std::function<bool()> declared;
		};

		/** @brief Each module epoch's passes, by RenderGraphRuntime::Segment value. Render thread writes; builds read a copy. */
		struct ForeignTable
		{
			std::mutex mutex;
			std::map<std::uint32_t, std::shared_ptr<const ForeignPasses>> entries;
		};

		ForeignTable& Foreign()
		{
			static ForeignTable table;
			return table;
		}

		std::shared_ptr<const ForeignPasses> ForeignOf(Segment a_segment)
		{
			auto& table = Foreign();
			const std::lock_guard lock(table.mutex);
			const auto found = table.entries.find(static_cast<std::uint32_t>(a_segment));
			return found != table.entries.end() ? found->second : nullptr;
		}

		class AnchorExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			explicit AnchorExtension(std::shared_ptr<const Snapshot> a_snapshot) :
				snapshot(std::move(a_snapshot)) {}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				if (!snapshot)
					return;
				const std::uint32_t epoch = a_graph.PassEpochFilter();
				const Anchor anchor = AnchorOfEpoch(epoch);
				const std::uint32_t declared = anchor == Anchor::Count ? DeclaredOfEpoch(epoch) : kDeclared;
				if (anchor == Anchor::Count && declared == kDeclared)
					return;
				// What the run that submits this graph reads (ClientDesc::executed, one-shot passes): the graph holds this snapshot's passes.
				// Recorded when the graph is installed: at its attach for a build the build manager makes (on its own thread, and possibly
				// dropped), at once for one the host makes on the owner thread. A failed managed build drops its clients (DropFailedClients).
				if (declared != kDeclared) {
					// A declared segment's graph: what it was built from is the segment's, and each member anchor's.
					// The passes this build makes (GatherDeclared fills it): their revisions are part of the segment's description.
					declaredPasses = std::make_shared<std::vector<std::weak_ptr<org::RenderPass>>>();
					// The stage's key as the build starts: the passes it hands to ORG (GatherDeclared).
					key = Anchors().nextKeys[declared].load(std::memory_order_acquire);
					const auto commit = [declared, built = snapshot, passes = declaredPasses, built_key = key] {
						auto& anchors = Anchors();
						anchors.declaredPrevious[declared].store(anchors.declaredSnapshots[declared].load(std::memory_order_acquire), std::memory_order_release);
						anchors.declaredSnapshots[declared].store(built, std::memory_order_release);
						anchors.declaredPasses[declared].store(passes, std::memory_order_release);
						anchors.declaredKeys[declared].store(built_key, std::memory_order_release);
						for (std::uint32_t i = 0; i < kAnchors; ++i)
							if (WholeMembers(declared) >> i & 1u) {
								anchors.built[i].store(built->serial, std::memory_order_release);
								anchors.previousSnapshots[i].store(anchors.builtSnapshots[i].load(std::memory_order_acquire), std::memory_order_release);
								anchors.builtSnapshots[i].store(built, std::memory_order_release);
							}
					};
					const auto onFailed = [declared, built = snapshot](const std::string& a_error) {
						DropFailedClients(StepAnchors(declared), kDeclaredInfo[declared].names.label, built, a_error);
					};
					// A staged build: its snapshot and passes describe the frames it is recorded for before its attach.
					(void)RenderGraph::Builds::DeferStaged([declared, built = snapshot, passes = declaredPasses, built_key = key] {
						auto& anchors = Anchors();
						anchors.stagedSnapshots[declared].store(built, std::memory_order_release);
						anchors.stagedPasses[declared].store(passes, std::memory_order_release);
						anchors.stagedKeys[declared].store(built_key, std::memory_order_release);
					});
					if (!RenderGraph::Builds::Defer(commit, onFailed))
						commit();
				} else {
					const auto index = IndexOf(anchor);
					const auto commit = [index, built = snapshot] {
						auto& anchors = Anchors();
						anchors.built[index].store(built->serial, std::memory_order_release);
						anchors.previousSnapshots[index].store(anchors.builtSnapshots[index].load(std::memory_order_acquire), std::memory_order_release);
						anchors.builtSnapshots[index].store(built, std::memory_order_release);
					};
					const auto onFailed = [index, built = snapshot](const std::string& a_error) { DropFailedClients(1u << index, kInfo[index].name, built, a_error); };
					if (!RenderGraph::Builds::Defer(commit, onFailed))
						commit();
				}
				// Each import's slot is the id's resolver, so a declaration by id follows its versions too.
				for (const auto& [id, slot] : snapshot->imports)
					if (const org::ResourceIdentifier identifier(id); !a_graph._resolverMap.contains(identifier))
						a_graph.RegisterResolver(identifier, slot);
				for (const auto& client : snapshot->clients)
					if (client->registerResources)
						client->registerResources(a_graph);
				if (declared != kDeclared ? SegmentUsesSharedConstants(*snapshot, declared) : UsesSharedConstantsIn(snapshot->clients, AnchorsIn(anchor, declared)))
					SharedConstants::Register(a_graph);
			}

			void GatherStructuralPasses(org::RenderGraph& a_graph, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				if (!snapshot)
					return;
				const std::uint32_t epoch = a_graph.PassEpochFilter();
				const Anchor anchor = AnchorOfEpoch(epoch);
				const std::uint32_t declared = anchor == Anchor::Count ? DeclaredOfEpoch(epoch) : kDeclared;
				const std::uint32_t anchors = AnchorsIn(anchor, declared);
				if (!anchors)
					return;
				if (declared != kDeclared) {
					GatherDeclared(a_graph, declared, epoch, a_out);
					return;
				}
				// The shared constants' upload first: the passes that read them follow it (the graph orders them by the buffers).
				if (UsesSharedConstantsIn(snapshot->clients, anchors))
					if (auto upload = SharedConstants::MakeUploadPass())
						a_out.push_back(std::move(org::RenderGraph::ExternalPassDesc::Copy("cs.shared.upload", std::move(upload))
								.PreferQueue(org::QueueKind::Graphics)
								.Epoch(epoch)));
				std::vector<const PassDesc*> passes;
				for (const auto& scheduled : snapshot->passes)
					if ((anchors >> IndexOf(scheduled.pass->anchor) & 1u) && !scheduled.client->segmentOnly)
						passes.push_back(scheduled.pass);
				for (const auto* scheduled : passes) {
					const auto& pass = *scheduled;
					auto made = pass.make ? pass.make() : pass.makeInstance ? pass.makeInstance(0) : nullptr;
					if (!made)
						continue;
					auto desc = pass.kind == PassKind::Compute ? org::RenderGraph::ExternalPassDesc::Compute(pass.name, std::move(made)) :
					            pass.kind == PassKind::Copy    ? org::RenderGraph::ExternalPassDesc::Copy(pass.name, std::move(made)) :
					                                             org::RenderGraph::ExternalPassDesc::Render(pass.name, std::move(made));
					// Beside the D3D11 work around the anchor: off the graphics queue, a pass would add a wait on DXVK's stream.
					if (pass.queue == PassQueue::Compute)
						desc.PreferQueue(org::QueueKind::Compute);
					else if (pass.kind != PassKind::Render)
						desc.PreferQueue(org::QueueKind::Graphics);
					// Recorded after the run's commit, on the graphics queue only (PassDesc::atExecution): in DXVK's stream (guest), or inline
					// into the epoch's own list (native).
					// A GPU-gated pass is recorded ahead instead: its closed entries make its indirect counts zero (GpuGates).
					if (pass.atExecution && !(pass.gpuGated && GpuGates()) && pass.queue == PassQueue::Default && a_graph.PassEpochRecordsAtExecution())
						desc.RecordAtExecution();
					a_out.push_back(std::move(desc.Epoch(epoch)));
				}
			}

		private:
			/** @brief The anchors whose passes this snapshot puts in the graph: an anchor's own, or a declared segment's members. */
			std::uint32_t AnchorsIn(Anchor a_anchor, std::uint32_t a_declared) const
			{
				if (a_anchor != Anchor::Count)
					return 1u << IndexOf(a_anchor);
				if (a_declared != kDeclared)
					return StepAnchors(a_declared);
				return 0;
			}

			/**
			 * @brief A declared segment's graph: its steps' passes in the steps' order, each anchor's by order key. Before a step whose
			 * clients read CS's shared constants, an upload of the values its tap saw (SharedConstants::MakeUploadPass with the step's
			 * index). Each pass is off in a recording whose description turns its client off (SetSkipWhen): it records only its barriers
			 * then. A pass marked atExecution (an open draw list, a pass that reads its run's slots or gate answers when it records) is
			 * recorded at execution, as the segment's late batch, inline at the segment's submission, unless its draws are GPU-gated. A
			 * late batch of gated copies and clears alone keeps its list while its gate answers repeat (CS_ORG_KEEP_GATED).
			 */
			void GatherDeclared(org::RenderGraph& a_graph, std::uint32_t a_declared, std::uint32_t a_epoch, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) const
			{
				const auto& clients = SegmentClients(*snapshot, a_declared);
				const auto& info = kDeclaredInfo[a_declared];
				const auto note = [&](const std::shared_ptr<org::RenderPass>& a_pass) {
					if (declaredPasses && a_pass)
						declaredPasses->push_back(a_pass);
				};
				// The build's key: a pass it leaves out is not handed to ORG at all.
				const BuildKey* const buildKey = key.get();
				const auto firstPass = a_out.size();
				// The stage's pass count, once per build (the per-kind check: a frame kind's stage carries only its own passes).
				struct CountLine
				{
					const std::vector<org::RenderGraph::ExternalPassDesc>& out;
					std::size_t first;
					const char* label;
					~CountLine() { logger::info("[ORG] frame segments: a build of the {} holds {} passes", label, out.size() - first); }
				} countLine{ a_out, firstPass, info.names.label };
				for (std::uint32_t k = 0; k < info.stepCount; ++k) {
					const auto& step = info.steps[k];
					if (step.foreign != Segment::Count) {
						const auto foreign = ForeignOf(step.foreign);
						if (!foreign || !foreign->gather || !(KeyForeign(buildKey, ~0u) >> k & 1u))
							continue;
						const auto first = a_out.size();
						foreign->gather(a_epoch, &a_out);
						for (auto i = first; i < a_out.size(); ++i) {
							auto& desc = a_out[i];
							desc.epoch = a_epoch;
							if (!desc.pass)
								continue;
							desc.pass->SetSkipWhen([a_declared, k](const org::IHostExecutionData* a_data) {
								const auto* data = a_data ? a_data->Get<SegmentHostData>() : nullptr;
								return data && data->segment == a_declared && !(data->fields.foreignOn >> k & 1u);
							});
							if (!desc.recordAtExecution)
								note(desc.pass);
						}
						continue;
					}
					const bool uploads = std::ranges::any_of(snapshot->clients, [&](const auto& a_client) {
						return a_client->sharedConstants && StepHolds(step, *a_client) && KeyHas(buildKey, *a_client, -1);
					});
					if (uploads)
						if (auto upload = SharedConstants::MakeUploadPass(k)) {
							note(upload);
							a_out.push_back(std::move(org::RenderGraph::ExternalPassDesc::Copy(fmt::format("cs.shared.upload.{}", k), std::move(upload))
									.PreferQueue(org::QueueKind::Graphics)
									.Epoch(a_epoch)));
						}
					// One pass instance, in the graph's order.
					const auto emit = [&](const ScheduledPass& scheduled, std::uint32_t instance, bool instanced) {
						const auto& pass = *scheduled.pass;
						if (!KeyHas(buildKey, *scheduled.client, pass.row))
							return;
						const auto found = std::ranges::find(clients, scheduled.client);
						const auto bit = static_cast<std::uint32_t>(found - clients.begin());
						auto made = instanced ? pass.makeInstance(instance) : pass.make ? pass.make() : pass.makeInstance ? pass.makeInstance(0) : nullptr;
						if (!made)
							return;
						made->SetSkipWhen([a_declared, bit, instanced, instance, row = pass.row](const org::IHostExecutionData* a_data) {
							const auto* data = a_data ? a_data->Get<SegmentHostData>() : nullptr;
							if (!data || data->segment != a_declared)
								return false;
							// A row the description did not select records only its barriers.
							if (row >= 0 && row < 256 && !(data->fields.rows[static_cast<std::size_t>(row) / 64] >> (static_cast<std::uint32_t>(row) % 64) & 1u))
								return true;
							// A client past the described ones is always on, as before instances.
							if (bit >= RenderGraph::Frame::kSegmentClients)
								return false;
							if (!(data->fields.clientsOn >> bit & 1u))
								return true;
							return instanced && instance >= data->fields.instances[bit];
						});
						// Recorded at execution as on an anchor: a gated pass whose draws are not indirect still reads its run's gate
						// answers (and often its run's slots) when it records. A GPU-gated pass is recorded ahead.
						const bool live = pass.atExecution && !(pass.gpuGated && GpuGates()) && pass.queue == PassQueue::Default && a_graph.PassEpochRecordsAtExecution();
						if (!live)
							note(made);
						std::string name = instanced ? fmt::format("{}.i{}", pass.name, instance) : pass.name;
						auto desc = pass.kind == PassKind::Compute ? org::RenderGraph::ExternalPassDesc::Compute(std::move(name), std::move(made)) :
						            pass.kind == PassKind::Copy    ? org::RenderGraph::ExternalPassDesc::Copy(std::move(name), std::move(made)) :
						                                             org::RenderGraph::ExternalPassDesc::Render(std::move(name), std::move(made));
						if (pass.queue == PassQueue::Compute)
							desc.PreferQueue(org::QueueKind::Compute);
						else if (pass.kind != PassKind::Render)
							desc.PreferQueue(org::QueueKind::Graphics);
						if (live)
							desc.RecordAtExecution();
						a_out.push_back(std::move(desc.Epoch(a_epoch)));
					};
					std::vector<const ClientDesc*> major;  // instanceMajor clients already made at this step
					for (const auto& scheduled : snapshot->passes) {
						if (scheduled.pass->anchor != step.anchor || !StepHolds(step, *scheduled.client))
							continue;
						// A client with instances has its pass made once per instance. Instance k is off in a recording whose
						// description declares k instances or fewer.
						const bool instanced = scheduled.client->maxInstances > 1 && scheduled.pass->makeInstance;
						// The step's instances of the client: its range (StepInstances).
						const auto [first, count] = instanced ? StepInstances(step, *scheduled.client) : std::pair<std::uint32_t, std::uint32_t>{ 0, 1 };
						if (instanced && scheduled.client->instanceMajor) {
							// Every pass of the step's first instance, then of the next, ...: the taps' order across the client's passes.
							if (std::ranges::find(major, scheduled.client.get()) != major.end())
								continue;
							major.push_back(scheduled.client.get());
							for (std::uint32_t instance = first; instance < first + count; ++instance)
								for (const auto& other : snapshot->passes)
									if (other.client == scheduled.client && other.pass->anchor == step.anchor && other.pass->makeInstance)
										emit(other, instance, true);
							continue;
						}
						for (std::uint32_t instance = first; instance < first + count; ++instance)
							emit(scheduled, instance, instanced);
					}
				}
			}

			// A declared segment's graph: the passes it made, kept with the build (State::declaredPasses), and the key it was built with.
			std::shared_ptr<std::vector<std::weak_ptr<org::RenderPass>>> declaredPasses;
			std::shared_ptr<const BuildKey> key;
			std::shared_ptr<const Snapshot> snapshot;
		};

		/** @brief The import's description: its views as their owner (the engine, a feature) made them. Any view may be null. */
		org::TextureDescription DescribeColour(const D3D11_TEXTURE2D_DESC& a_desc, ID3D11ShaderResourceView* a_srv, ID3D11RenderTargetView* a_rtv,
			ID3D11UnorderedAccessView* a_uav)
		{
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(a_desc.Format);
			desc.channels = 4;
			if (a_srv) {
				D3D11_SHADER_RESOURCE_VIEW_DESC view{};
				a_srv->GetDesc(&view);
				desc.hasSRV = true;
				desc.srvFormat = rhi::helpers::ToRHI(view.Format);
				desc.format = desc.srvFormat;
			}
			if (a_rtv) {
				D3D11_RENDER_TARGET_VIEW_DESC view{};
				a_rtv->GetDesc(&view);
				desc.hasRTV = true;
				desc.rtvFormat = rhi::helpers::ToRHI(view.Format);
			}
			if (a_uav) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
				a_uav->GetDesc(&view);
				desc.hasUAV = true;
				desc.uavFormat = rhi::helpers::ToRHI(view.Format);
				// A CPU-side UAV too, so a pass can clear it (AnchorPasses::Clear), as GraphResources' twins have.
				desc.hasNonShaderVisibleUAV = true;
			}
			return desc;
		}

		org::TextureDescription DescribeDepth(const RE::BSGraphics::DepthStencilData& a_target, const D3D11_TEXTURE2D_DESC& a_desc)
		{
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(a_desc.Format);
			desc.channels = 1;
			if (a_target.views[0]) {
				D3D11_DEPTH_STENCIL_VIEW_DESC view{};
				a_target.views[0]->GetDesc(&view);
				desc.hasDSV = true;
				desc.dsvFormat = rhi::helpers::ToRHI(view.Format);
				desc.format = desc.dsvFormat;
			}
			if (a_target.depthSRV) {
				D3D11_SHADER_RESOURCE_VIEW_DESC view{};
				a_target.depthSRV->GetDesc(&view);
				desc.hasSRV = true;
				desc.srvFormat = rhi::helpers::ToRHI(view.Format);
			}
			return desc;
		}

		/** @brief Width, height and format of a version's backing: an import (ExternalTextureResource) or a texture ORG made (PixelBuffer). */
		std::uint32_t WidthOf(const org::Resource* a_resource)
		{
			if (const auto* external = dynamic_cast<const org::ExternalTextureResource*>(a_resource))
				return external->GetWidth();
			if (const auto* texture = dynamic_cast<const org::PixelBuffer*>(a_resource))
				return texture->GetWidth();
			return 0;
		}

		std::uint32_t HeightOf(const org::Resource* a_resource)
		{
			if (const auto* external = dynamic_cast<const org::ExternalTextureResource*>(a_resource))
				return external->GetHeight();
			if (const auto* texture = dynamic_cast<const org::PixelBuffer*>(a_resource))
				return texture->GetHeight();
			return 0;
		}

		rhi::Format FormatOf(const org::Resource* a_resource)
		{
			if (const auto* external = dynamic_cast<const org::ExternalTextureResource*>(a_resource))
				return external->GetFormat();
			if (const auto* texture = dynamic_cast<const org::PixelBuffer*>(a_resource))
				return texture->GetFormat();
			return rhi::Format::Unknown;
		}

		/** @brief A view's format as the backing makes it: an unnamed one is the description's own format. */
		rhi::Format ViewFormat(rhi::Format a_view, const org::TextureDescription& a_desc)
		{
			return a_view != rhi::Format::Unknown ? a_view : a_desc.format;
		}

		/** @brief Whether a backing with a_have's view serves one that asks for a_want's (an unnamed format asks for any). */
		bool HasView(bool a_have, rhi::Format a_haveFormat, bool a_want, rhi::Format a_wantFormat)
		{
			return !a_want || (a_have && (a_wantFormat == rhi::Format::Unknown || a_haveFormat == a_wantFormat));
		}

		/** @brief Whether a backing described by a_have has every view a_want asks for, in its format. */
		bool Covers(const org::TextureDescription& a_have, const org::TextureDescription& a_want)
		{
			return HasView(a_have.hasSRV, ViewFormat(a_have.srvFormat, a_have), a_want.hasSRV, a_want.srvFormat) &&
			       HasView(a_have.hasRTV, ViewFormat(a_have.rtvFormat, a_have), a_want.hasRTV, a_want.rtvFormat) &&
			       HasView(a_have.hasDSV, ViewFormat(a_have.dsvFormat, a_have), a_want.hasDSV, a_want.dsvFormat) &&
			       HasView(a_have.hasUAV, ViewFormat(a_have.uavFormat, a_have), a_want.hasUAV, a_want.uavFormat) &&
			       (!a_want.hasNonShaderVisibleUAV || a_have.hasNonShaderVisibleUAV) && (!a_want.isCubemap || a_have.isCubemap);
		}

		/**
		 * @brief a_have with the views of a_want it lacks. Its format stays a_have's, so the result is the same shape class. A view both
		 * have in different formats keeps a_have's: a_conflict names it then.
		 */
		org::TextureDescription MergeViews(const org::TextureDescription& a_have, const org::TextureDescription& a_want, std::string& a_conflict)
		{
			auto merged = a_have;
			const auto add = [&](bool& a_mergedHas, rhi::Format& a_mergedFormat, bool a_wantHas, rhi::Format a_wantFormat, const char* a_name) {
				if (!a_wantHas)
					return;
				if (!a_mergedHas) {
					a_mergedHas = true;
					a_mergedFormat = a_wantFormat;
				} else if (a_wantFormat != rhi::Format::Unknown && ViewFormat(a_mergedFormat, merged) != a_wantFormat) {
					a_conflict = fmt::format("{}{}{} (format {} asked, {} kept)", a_conflict, a_conflict.empty() ? "" : ", ", a_name, static_cast<int>(a_wantFormat),
						static_cast<int>(ViewFormat(a_mergedFormat, merged)));
				}
			};
			add(merged.hasSRV, merged.srvFormat, a_want.hasSRV, a_want.srvFormat, "SRV");
			add(merged.hasRTV, merged.rtvFormat, a_want.hasRTV, a_want.rtvFormat, "RTV");
			add(merged.hasDSV, merged.dsvFormat, a_want.hasDSV, a_want.dsvFormat, "DSV");
			add(merged.hasUAV, merged.uavFormat, a_want.hasUAV, a_want.uavFormat, "UAV");
			merged.hasNonShaderVisibleUAV = merged.hasNonShaderVisibleUAV || a_want.hasNonShaderVisibleUAV;
			merged.isCubemap = merged.isCubemap || a_want.isCubemap;
			if (merged.channels == 0)
				merged.channels = a_want.channels;
			return merged;
		}

		/**
		 * @brief a_desc's subresources from the image itself, the same for every importer: one entry per mip, and its slices (in cubes
		 * when a_desc asks for a cube and the slices make whole cubes).
		 */
		void DescribeSubresources(org::TextureDescription& a_desc, const DxvkOrgInteropImageInfo& a_image)
		{
			a_desc.imageDimensions.clear();
			const std::uint32_t mips = (std::max)(a_image.mipLevels, 1u);
			for (std::uint32_t mip = 0; mip < mips; ++mip)
				a_desc.imageDimensions.push_back({ (std::max)(a_image.extent.width >> mip, 1u), (std::max)(a_image.extent.height >> mip, 1u), 0, 0 });
			const std::uint32_t layers = (std::max)(a_image.arrayLayers, 1u);
			a_desc.isCubemap = a_desc.isCubemap && layers % 6 == 0;
			const std::uint32_t slices = a_desc.isCubemap ? layers / 6 : layers;
			a_desc.isArray = slices > 1;
			a_desc.arraySize = slices;
		}

		/** @brief The first 16 of each kind of import line: these repeat for a texture made again. */
		bool LogImportLine(std::atomic<std::uint32_t>& a_count)
		{
			return a_count.fetch_add(1, std::memory_order_relaxed) < 16;
		}

		/**
		 * @brief The backing of a new slot for a_image with a_desc's views (a_desc gets the views the backing has). Native mode: the
		 * image's graph-owned texture (Engine::Native::GraphTextureOf; a_texture may be null, then it is looked up by image), always. It
		 * has every view its D3D11 bind flags allow, so an importer can only ask for one in another format (logged; it gets the
		 * texture's), and importing the image again would only make a second graph resource of it. Otherwise the image imported. The one
		 * place an image becomes a graph resource for the anchors and DCLF. Null, logged, on failure.
		 */
		std::shared_ptr<org::GloballyIndexedResource> MakeBacking(ID3D11Texture2D* a_texture, const DxvkOrgInteropImageInfo& a_image,
			org::TextureDescription& a_desc, const std::string& a_id)
		{
			auto& runtime = RenderGraphRuntime::Get();
			const auto image = reinterpret_cast<std::uint64_t>(a_image.image);
			if (RenderGraph::Engine::Native::Active()) {
				auto owned = a_texture ? RenderGraph::Engine::Native::GraphTextureOf(a_texture) : RenderGraph::Engine::Native::GraphTextureOfImage(image);
				if (const auto* texture = dynamic_cast<const org::PixelBuffer*>(owned.get())) {
					const auto& made = texture->GetDescription();
					if (!Covers(made, a_desc)) {
						std::string conflict;
						const auto merged = MergeViews(made, a_desc, conflict);
						static std::atomic<std::uint32_t> logged{ 0 };
						if (LogImportLine(logged))
							logger::warn("[ORG] frame anchors: {}: the graph-owned texture {}{}{}; it stays the image's one graph resource", a_id,
								Covers(made, merged) ? "has the views asked for in other formats" : "lacks a view asked for, which its bind flags do not allow",
								conflict.empty() ? "" : ": ", conflict);
					}
					a_desc = made;
					DescribeSubresources(a_desc, a_image);
					return owned;
				}
			}
			DescribeSubresources(a_desc, a_image);
			auto resource = RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), a_image, a_desc, a_id.c_str());
			if (!resource)
				logger::warn("[ORG] frame anchors: {} could not be imported", a_id);
			return resource;
		}

		/**
		 * @brief The graph resource an image ORG made already has (RenderGraph::MadeImageOf: a GraphResources twin's export, an engine
		 * texture, a streaming window, native mode's exempt textures): the twin's slot when it is versioned, else the resource itself,
		 * declared as itself. Null when the image is not one of those, or when its resource lacks a view a_desc asks for (logged): the
		 * caller imports the image then, and the duplicate check names that second resource.
		 */
		Texture MadeTexture(ID3D11Texture2D* a_texture, std::uint64_t a_image, const org::TextureDescription& a_desc, std::string_view a_by)
		{
			// Native mode: an exempt texture's graph resource is made on its first lookup (ImmutableTextureOf), which registers it. A
			// read-only importer looks it up first, so the image keeps that one resource whoever asks first.
			if (a_texture && RenderGraph::Engine::Native::Active() && !a_desc.hasRTV && !a_desc.hasUAV && !a_desc.hasDSV)
				(void)RenderGraph::Engine::Native::ImmutableTextureOf(a_texture, DXGI_FORMAT_UNKNOWN);
			auto made = RenderGraph::MadeImageOf(a_image);
			if (!made.resource)
				return {};
			if (made.views && !Covers(*made.views, a_desc)) {
				static std::atomic<std::uint32_t> loggedLacking{ 0 };
				if (LogImportLine(loggedLacking))
					logger::warn("[ORG] frame anchors: {} asks for a view '{}' lacks; its image is imported again, a second graph resource of it", a_by, made.name);
				return {};
			}
			static std::atomic<std::uint32_t> loggedFound{ 0 };
			if (LogImportLine(loggedFound))
				logger::info("[ORG] frame anchors: {} is the image of '{}', which ORG made; it is that graph resource{}", a_by, made.name,
					made.slot ? "'s slot" : "");
			if (made.slot)
				return Texture(made.name, std::move(made.slot));
			return Texture(std::move(made.resource));
		}

		/** @brief How a new backing joined an import's slot. */
		enum class Joined : std::uint8_t
		{
			Version,   // the slot's next version, the same size: nothing is built again
			Resized,   // the slot's next version, another size: passes that took the size at their build are made again
			NewShape,  // another format, mip count or slice count: a new slot, and graphs built with the old one are built again
		};

		/**
		 * @brief Publishes a_next as a_slot's next version when it has the slot's shape class, kept alive with a_owner. Tickets prepared
		 * for the old version are prepared again (NoteNewVersions). Render thread.
		 */
		Joined JoinSlot(org::VersionedTexture& a_slot, std::shared_ptr<org::GloballyIndexedResource> a_next, std::shared_ptr<const void> a_owner = {})
		{
			const auto previous = a_slot.CurrentResource();
			const bool sameSize = previous && WidthOf(previous.get()) == WidthOf(a_next.get()) && HeightOf(previous.get()) == HeightOf(a_next.get());
			try {
				a_slot.PublishImage(std::move(a_next), std::move(a_owner));
			} catch (const std::exception&) {
				return Joined::NewShape;
			}
			if (auto* host = RenderGraphRuntime::Get().Host())
				host->NoteNewVersions();
			return sameSize ? Joined::Version : Joined::Resized;
		}

		/** @brief The entry whose current version is a_image: an id import, else a loose slot still held. Null when there is none. */
		std::shared_ptr<EngineImport> FindImage(State& a_state, std::uint64_t a_image)
		{
			if (!a_image)
				return nullptr;
			for (const auto& [id, import] : a_state.imports)
				if (import->image == a_image)
					return import;
			std::erase_if(a_state.loose, [](const std::weak_ptr<EngineImport>& a_entry) {
				const auto entry = a_entry.lock();
				return !entry || entry->looseSlot.expired();
			});
			for (const auto& weak : a_state.loose)
				if (auto entry = weak.lock(); entry && entry->image == a_image)
					return entry;
			return nullptr;
		}

		/** @brief Whether an id other than a_except names a_import. */
		bool NamedElsewhere(const State& a_state, const std::shared_ptr<EngineImport>& a_import, std::string_view a_except)
		{
			return std::ranges::any_of(a_state.imports, [&](const auto& a_entry) { return a_entry.first != a_except && a_entry.second == a_import; });
		}

		/**
		 * @brief Makes a_import's slot serve a_want's views too: when its current version lacks one, the slot's next version is the same
		 * image imported again with both sets. Passes keep declaring the slot, so nothing is built again. A view both name in different
		 * formats keeps the slot's (logged). Render thread.
		 */
		void EnsureViews(const std::shared_ptr<EngineImport>& a_import, const org::TextureDescription& a_want, std::string_view a_by)
		{
			if (Covers(a_import->views, a_want))
				return;
			auto slot = a_import->Slot();
			if (!slot)
				return;
			std::string conflict;
			auto merged = MergeViews(a_import->views, a_want, conflict);
			if (!conflict.empty()) {
				static std::atomic<std::uint32_t> logged{ 0 };
				if (LogImportLine(logged))
					logger::warn("[ORG] frame anchors: {} asks for a view of {}'s image in another format: {}; it gets the slot's", a_by, a_import->id, conflict);
			}
			if (Covers(a_import->views, merged))
				return;
			const auto old = slot->CurrentResource();
			// Native mode's graph-owned texture has every view its bind flags allow: importing its image again would add nothing but a
			// second graph resource of it.
			if (dynamic_cast<const org::PixelBuffer*>(old.get())) {
				static std::atomic<std::uint32_t> logged{ 0 };
				if (LogImportLine(logged))
					logger::warn("[ORG] frame anchors: {} asks {}'s graph-owned texture for a view its bind flags do not allow; it keeps its views", a_by, a_import->id);
				return;
			}
			DescribeSubresources(merged, a_import->info);
			auto& runtime = RenderGraphRuntime::Get();
			// The old version stops being the image's resource here: frames already prepared with it finish with it.
			RenderGraph::RetireImageResource(old.get());
			auto next = RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), a_import->info, merged, a_import->id.c_str());
			// A loose slot's versions keep its entry, and with it the D3D11 texture.
			std::shared_ptr<const void> owner;
			if (!a_import->slot)
				owner = a_import;
			if (!next || JoinSlot(*slot, next, owner) == Joined::NewShape) {
				logger::warn("[ORG] frame anchors: {}'s image could not be imported again with the views {} asks for; it keeps the slot's views", a_import->id, a_by);
				if (old)
					RenderGraph::NoteImageResource(a_import->image, old, a_import->id);
				return;
			}
			a_import->views = std::move(merged);
			static std::atomic<std::uint32_t> logged{ 0 };
			if (LogImportLine(logged))
				logger::info("[ORG] frame anchors: {}'s slot has the views {} asks for now: a version of the same image", a_import->id, a_by);
		}

		/** @brief Drops a_import once nothing names it: its D3D11 texture goes once D3D11's last use is done. */
		void ReleaseUnnamed(State& a_state, std::shared_ptr<EngineImport> a_import)
		{
			if (a_import && !NamedElsewhere(a_state, a_import, {}))
				RenderGraphRuntime::Get().ReleaseAfterD3D11(std::move(a_import));
		}

		Texture Import(ID3D11Texture2D* a_texture, const std::string& a_id,
			const std::function<org::TextureDescription(const D3D11_TEXTURE2D_DESC&)>& a_describe)
		{
			auto lock = Guard();
			auto& state = Anchors();
			auto& runtime = RenderGraphRuntime::Get();
			if (!a_texture || !runtime.IsActive())
				return {};
			// An engine target ORG owns (CS_ORG_ENGINE_TARGETS): its published slot, which every graph registers. Importing the image
			// again would make a second resource whose uses the graph does not order with the owner's.
			if (auto owned = RenderGraph::Engine::Targets::Find(a_texture))
				return owned;
			// Anything else is imported as DXVK's own texture, never as a tripwire handle. Native mode has no DXVK texture under its
			// handles: its own records describe the handle itself.
			if (!RenderGraph::Engine::Native::Active())
				a_texture = RenderGraph::Engine::Tripwire::Unwrap(a_texture);
			const auto found = state.imports.find(a_id);
			const std::shared_ptr<EngineImport> previous = found != state.imports.end() ? found->second : nullptr;
			if (previous && previous->texture.get() == a_texture)
				return Texture(a_id, previous->slot);

			DxvkOrgInteropResourceInfo info{};
			if (!runtime.DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE) {
				logger::warn("[ORG] frame anchors: {} cannot be described", a_id);
				return {};
			}
			D3D11_TEXTURE2D_DESC textureDesc{};
			a_texture->GetDesc(&textureDesc);
			auto desc = a_describe(textureDesc);
			// A cube (the engine's cubemap targets): its SRV is a cube view, and arraySize counts cubes.
			desc.isCubemap = (textureDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
			DescribeSubresources(desc, info.image);
			const auto image = reinterpret_cast<std::uint64_t>(info.image.image);

			// An image ORG made with a graph resource of its own (a twin's export in an engine slot, an engine texture): that resource.
			// The id keeps whatever slot it had; this texture is not a version of it.
			if (auto made = MadeTexture(a_texture, image, desc, a_id))
				return made;

			// One image, one graph resource: an image some importer already has keeps its slot, whoever came first. Two graph resources
			// of one image would each track its state, and their layout moves would race.
			if (auto shared = FindImage(state, image)) {
				EnsureViews(shared, desc, a_id);
				if (shared == previous) {
					// The same image under another handle (a port's handle around it).
					shared->texture.copy_from(a_texture);
					return Texture(a_id, shared->slot);
				}
				if (!shared->slot) {
					// A loose slot (ShareImage's) becomes this id's: the state holds it from now on, through an entry of its own. The loose
					// entry stays its versions' owner (they hold it, so it cannot hold the slot).
					auto taken = std::make_shared<EngineImport>(*shared);
					taken->slot = shared->looseSlot.lock();
					taken->looseSlot.reset();
					std::erase_if(state.loose, [&](const std::weak_ptr<EngineImport>& a_entry) { return a_entry.lock() == shared; });
					shared = std::move(taken);
				}
				if (!shared->texture)
					shared->texture.copy_from(a_texture);
				logger::info("[ORG] frame anchors: {} is the image {} has already; it shares that slot", a_id, shared->id);
				state.imports[a_id] = shared;
				Publish(state);
				if (previous) {
					ReleaseUnnamed(state, previous);
					StructuralRebuild(state, ~0u, fmt::format("import '{}' now shares the slot of '{}'", a_id, shared->id));
				} else if (state.host) {
					RenderGraph::Builds::RequestFor(a_id, fmt::format("frame anchors: import '{}' added", a_id));
				}
				return Texture(a_id, shared->slot);
			}

			auto backing = MakeBacking(a_texture, info.image, desc, a_id);
			if (!backing)
				return {};
			// A new texture for an id that alone names its slot: the slot's next version. A slot another id shares stays that id's; this
			// one gets a slot of its own.
			if (previous && !NamedElsewhere(state, previous, a_id)) {
				const auto joined = JoinSlot(*previous->slot, backing);
				if (joined != Joined::NewShape) {
					// The old D3D11 texture goes once D3D11's last use is done. The slot's old version keeps its graph resource while a
					// frame or a prepared ticket holds it.
					runtime.ReleaseAfterD3D11(std::make_shared<winrt::com_ptr<ID3D11Texture2D>>(std::move(previous->texture)));
					previous->texture.copy_from(a_texture);
					previous->image = image;
					previous->info = info.image;
					previous->views = desc;
					if (joined == Joined::Resized)
						Rebuild(state, ~0u, fmt::format("import '{}' resized: passes that took its size are made again", a_id));
					return Texture(a_id, previous->slot);
				}
			}
			auto import = std::make_shared<EngineImport>();
			import->texture.copy_from(a_texture);
			import->slot = org::VersionedTexture::CreateImage(std::move(backing));
			import->id = a_id;
			import->image = image;
			import->info = info.image;
			import->views = desc;
			auto slot = import->slot;
			state.imports[a_id] = std::move(import);
			ReleaseUnnamed(state, previous);
			Publish(state);
			// The imports are registered with every anchor graph. A new one is built into the graphs that looked its id up (a client
			// registering with it builds its own anchors again). One of another shape class is a new slot: every graph holding the
			// old one is built again, off the render thread (the build manager).
			if (previous)
				StructuralRebuild(state, ~0u, fmt::format("import '{}' replaced by another shape class", a_id));
			else if (state.host)
				RenderGraph::Builds::RequestFor(a_id, fmt::format("frame anchors: import '{}' added", a_id));
			return Texture(a_id, std::move(slot));
		}
	}

	Texture::Texture(std::string a_id, std::shared_ptr<org::VersionedTexture> a_slot) :
		id(std::move(a_id)), slot(std::move(a_slot))
	{}

	std::shared_ptr<org::GloballyIndexedResource> Texture::Current() const
	{
		return std::dynamic_pointer_cast<org::GloballyIndexedResource>(slot ? slot->CurrentResource() : plain);
	}

	org::GloballyIndexedResource* Texture::operator->() const
	{
		// The slot keeps its current version alive until the render thread publishes the next one.
		return Current().get();
	}

	std::uint32_t Texture::Width() const { return WidthOf(Current().get()); }
	std::uint32_t Texture::Height() const { return HeightOf(Current().get()); }
	rhi::Format Texture::Format() const { return FormatOf(Current().get()); }

	bool Texture::SameBacking(const Texture& a_other) const
	{
		return Current() == a_other.Current();
	}

	const org::IResourceResolver* Texture::DeclaredResolver() const
	{
		return slot.get();
	}

	const char* Name(Anchor a_anchor)
	{
		return IndexOf(a_anchor) < kAnchors ? kInfo[IndexOf(a_anchor)].name : "unknown";
	}

	std::uint32_t SegmentOf(Anchor a_anchor)
	{
		return static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass) + IndexOf(a_anchor);
	}

	const SegmentNames* NamesOf(std::uint32_t a_segment)
	{
		for (const auto& declared : kDeclaredInfo)
			if (a_segment == static_cast<std::uint32_t>(declared.segment))
				return &declared.names;
		const std::uint32_t first = static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass);
		if (a_segment < first || a_segment - first >= kAnchors)
			return nullptr;
		return &kInfo[a_segment - first].names;
	}

	bool IsUnusedSegment(std::uint32_t a_segment)
	{
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (a_segment == static_cast<std::uint32_t>(kDeclaredInfo[d].segment)) {
				const auto& state = Anchors();
				return !state.frozen || !state.declaredEpochs[d];
			}
		const std::uint32_t first = static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass);
		if (a_segment < first || a_segment - first >= kAnchors)
			return false;
		const auto& state = Anchors();
		// A declared segment's member has no epoch of its own.
		return !state.frozen || !state.reserved[a_segment - first] || (state.declaredMembers >> (a_segment - first) & 1u);
	}

	bool Reserve(Anchor a_anchor)
	{
		auto& state = Anchors();
		if (IndexOf(a_anchor) >= kAnchors)
			return false;
		if (state.reserved[IndexOf(a_anchor)])
			return true;
		if (state.frozen) {
			logger::warn("[ORG] frame anchor {}: reserved after the render graph initialized; it has no epoch this session", Name(a_anchor));
			return false;
		}
		if (!RenderGraphRuntime::EpochsEnabled()) {
			logger::info("[ORG] frame anchor {}: not reserved, the render graph runs without epochs (CS_ORG_EPOCHS=0)", Name(a_anchor));
			return false;
		}
		state.reserved[IndexOf(a_anchor)] = true;
		return true;
	}

	bool HasEpoch(Anchor a_anchor)
	{
		const auto& state = Anchors();
		return IndexOf(a_anchor) < kAnchors && state.frozen && state.reserved[IndexOf(a_anchor)] && state.host && RenderGraphRuntime::Get().IsActive();
	}

	bool RecordsAtExecution(Anchor a_anchor)
	{
		// A declared segment records every pass ahead, on the host's thread.
		if (IndexOf(a_anchor) < kAnchors && DeclaredOfAnchor(Anchors(), IndexOf(a_anchor)) < kDeclared)
			return false;
		return HasEpoch(a_anchor) && RenderGraphRuntime::Get().RecordsAtExecution(EpochOfAnchor(a_anchor));
	}

	bool GpuGates()
	{
		// CS_ORG_GPU_GATES (native mode, default on, =0 off).
		static const bool on = Switches::Number("CS_ORG_GPU_GATES", 1) != 0;
		return on && RenderGraph::Engine::Native::Active();
	}

	bool Register(ClientDesc a_client)
	{
		auto lock = Guard();
		auto& state = Anchors();
		if (!state.host || !RenderGraphRuntime::Get().IsActive()) {
			logger::warn("[ORG] frame anchors: {} not registered, the render graph is not active", a_client.id);
			return false;
		}
		if (a_client.id.empty() || a_client.passes.empty()) {
			logger::warn("[ORG] frame anchors: a client without an id or passes ({}) is not registered", a_client.id);
			return false;
		}
		std::vector<std::string_view> names;
		for (const auto& pass : a_client.passes) {
			if (!HasEpoch(pass.anchor) || pass.name.empty() || (!pass.make && !pass.makeInstance) || std::ranges::find(names, pass.name) != names.end()) {
				logger::warn("[ORG] frame anchors: {} not registered: its pass '{}' at {} {}", a_client.id, pass.name, Name(pass.anchor),
					!HasEpoch(pass.anchor) ? "names an anchor without an epoch (reserve it before the graph initializes)" : "has no name, no make, or a repeated name");
				return false;
			}
			names.push_back(pass.name);
		}
		// Before CS's first upload the shared constants cannot be made yet: the client waits for them (Service).
		if (a_client.sharedConstants && !SharedConstants::Ensure() && SharedConstants::Failed()) {
			logger::warn("[ORG] frame anchors: {} not registered: it reads the shared constants, which could not be made", a_client.id);
			return false;
		}
		Client client;
		client.desc = std::make_shared<const ClientDesc>(std::move(a_client));
		for (const auto& pass : client.desc->passes)
			client.armed.push_back(pass.once ? 1 : 0);
		std::uint32_t mask = AnchorMask(*client.desc);
		auto* existing = FindClient(state, client.desc->id);
		std::shared_ptr<const ClientDesc> replaced;
		if (existing) {
			mask |= AnchorMask(*existing->desc);
			replaced = existing->desc;
			*existing = std::move(client);
		} else {
			existing = &state.clients.emplace_back(std::move(client));
		}
		Publish(state);
		MarkCurrent(state, *existing);
		if (replaced)
			RetireGates(state, *replaced, existing->desc->gates);
		Rebuild(state, mask, fmt::format("client '{}' registered{}", existing->desc->id, replaced ? " again" : ""));
		logger::info("[ORG] frame anchors: {} registered ({} passes){}", existing->desc->id, existing->desc->passes.size(),
			InGraphs(*existing->desc) ? "" : "; it joins the graphs once the shared constants exist");
		return true;
	}

	void Unregister(std::string_view a_id)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const auto existing = std::ranges::find_if(state.clients, [&](const Client& a_other) { return a_other.desc->id == a_id; });
		if (existing == state.clients.end())
			return;
		const std::uint32_t mask = AnchorMask(*existing->desc);
		const auto desc = existing->desc;
		state.clients.erase(existing);
		Publish(state);
		RetireGates(state, *desc);
		Rebuild(state, mask, fmt::format("client '{}' unregistered", desc->id));
	}

	void Refresh(std::string_view a_id)
	{
		auto lock = Guard();
		auto& state = Anchors();
		if (auto* client = FindClient(state, a_id)) {
			Publish(state);
			MarkCurrent(state, *client);
			Rebuild(state, AnchorMask(*client->desc), fmt::format("client '{}' refreshed", client->desc->id));
		}
	}

	bool InBuiltGraph(std::string_view a_id, Anchor a_anchor)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_anchor);
		const auto* client = FindClient(state, a_id);
		return client && index < kAnchors && client->serial <= BuiltSerial(state, index);
	}

	void Rearm(std::string_view a_id)
	{
		auto lock = Guard();
		auto& state = Anchors();
		auto* client = FindClient(state, a_id);
		if (!client)
			return;
		bool armed = false;
		for (std::size_t i = 0; i < client->desc->passes.size(); ++i)
			if (client->desc->passes[i].once && !client->armed[i]) {
				client->armed[i] = 1;
				armed = true;
			}
		if (!armed)
			return;
		Publish(state);
		for (std::size_t i = 0; i < client->desc->passes.size(); ++i)
			if (client->armed[i] == 1)
				client->armed[i] = state.serial;
		Rebuild(state, AnchorMask(*client->desc, true), fmt::format("client '{}' armed its one-shot passes", client->desc->id));
	}

	void SetLateEpochHook(std::function<void()> a_hook)
	{
		Anchors().lateHook = std::move(a_hook);
	}

	namespace
	{
		/**
		 * @brief Before a run: clients waiting for the shared constants join once they exist (or fail with them), and a twin's resize
		 * since the last run builds every anchor graph again and publishes the new resource.
		 */
		void Service(State& a_state)
		{
			const bool waiting = std::ranges::any_of(a_state.clients, [](const Client& a_client) { return !InGraphs(*a_client.desc); });
			if (waiting && SharedConstants::Ensure()) {
				Publish(a_state);
				std::uint32_t mask = 0;
				for (auto& client : a_state.clients)
					if (client.desc->sharedConstants) {
						MarkCurrent(a_state, client);
						mask |= AnchorMask(*client.desc);
					}
				Rebuild(a_state, mask, "the shared constants exist");
				logger::info("[ORG] frame anchors: the shared constants exist; the clients that read them join the graphs");
			} else if (waiting && SharedConstants::Failed()) {
				std::vector<std::shared_ptr<const ClientDesc>> failed;
				std::erase_if(a_state.clients, [&](const Client& a_client) {
					if (InGraphs(*a_client.desc))
						return false;
					failed.push_back(a_client.desc);
					return true;
				});
				Publish(a_state);
				for (const auto& client : failed) {
					RetireGates(a_state, *client);
					logger::error("[ORG] frame anchors: {} is removed: the shared constants it reads could not be made", client->id);
					if (client->onFailed)
						client->onFailed();
				}
			}
			const std::uint64_t generation = GraphResources::Resource::ResizeGeneration();
			if (generation != a_state.resizeGeneration) {
				// The log names what moved the generation since this state last looked.
				const auto causes = GraphResources::Resource::ResizeCauses(a_state.resizeGeneration);
				a_state.resizeGeneration = generation;
				PublishedResources::Refresh();
				Publish(a_state);
				for (auto& client : a_state.clients)
					MarkCurrent(a_state, client);
				StructuralRebuild(a_state, ~0u, fmt::format("graph resources resized by {} (a twin that is not versioned, or another shape class)", causes));
			}
		}

		/** @brief A client with work at this run: its desc, its serial then, and whether its commit runs (active). */
		struct RunClient
		{
			std::shared_ptr<const ClientDesc> desc;
			std::uint64_t serial = 0;
			bool active = false;
		};
	}

	namespace
	{
		std::atomic<DWORD> mainThread{ 0 };
		// With CS_ORG_LOADING_THREAD=1: the thread that owns rendering (0 until the first claim: the main thread), and the claims.
		std::atomic<DWORD> renderOwner{ 0 };
		std::atomic<std::uint64_t> ownerChanges{ 0 };

		/** @brief The thread that owns rendering now: the last claim's, or the main thread before any. */
		DWORD Owner()
		{
			const DWORD stored = renderOwner.load(std::memory_order_acquire);
			return stored ? stored : mainThread.load(std::memory_order_acquire);
		}
	}

	void LatchMainThread()
	{
		mainThread.store(::GetCurrentThreadId(), std::memory_order_release);
	}

	bool IsMainThread()
	{
		return ::GetCurrentThreadId() == mainThread.load(std::memory_order_acquire);
	}

	std::uint32_t RunSegment(Anchor a_anchor)
	{
		const auto index = IndexOf(a_anchor);
		if (index >= kAnchors)
			return static_cast<std::uint32_t>(Segment::Count);
		if (const auto d = DeclaredOfAnchor(Anchors(), index); d < kDeclared)
			return static_cast<std::uint32_t>(kDeclaredInfo[d].segment);
		return SegmentOf(a_anchor);
	}

	bool IsRenderOwner()
	{
		const DWORD main = mainThread.load(std::memory_order_acquire);
		const DWORD self = ::GetCurrentThreadId();
		if (main == 0)
			return true;
		if (!RenderGraph::Engine::LoadingScreen::Requested()) {
			// A loading screen presents from a job thread (SE 38085). The anchors' state and the graph's frame upload instance belong
			// to the main thread, so one-shot passes wait for the next main-thread frame.
			if (main == self)
				return true;
			static std::atomic_flag logged;
			if (!logged.test_and_set())
				logger::info("[ORG] frame anchors: a call on thread {} (a loading screen or a movie), not the main thread {}: the graph waits for the main thread",
					self, main);
			return false;
		}
		return Owner() == self;
	}

	void ClaimRenderOwnership()
	{
		if (!RenderGraph::Engine::LoadingScreen::Requested() || mainThread.load(std::memory_order_acquire) == 0)
			return;
		const DWORD self = ::GetCurrentThreadId();
		// Every engine frame begins here: most claims are by the thread that already owns rendering.
		if (Owner() == self)
			return;
		// The last owner's epochs still on the submission thread reach the queue before this thread's work. Before the anchors' lock:
		// a pass recorded on that thread may take it.
		RenderGraphRuntime::Get().DrainSubmissions("render ownership moved", true);
		// Under the anchors' mutex: no claim lands inside another thread's anchor run, and the main thread's calls outside a frame
		// (Register, imports) never interleave with it.
		auto lock = Guard();
		const DWORD previous = Owner();
		if (previous == self)
			return;
		renderOwner.store(self, std::memory_order_release);
		const auto changes = ownerChanges.fetch_add(1, std::memory_order_acq_rel) + 1;
		RenderGraphRuntime::Get().MoveOwnerThread();
		if (changes <= 32 || changes % 1000 == 0)
			logger::info("[ORG] render ownership: thread {} begins a frame and takes it from thread {} (change {})", self, previous, changes);
	}

	std::uint64_t RenderOwnerChanges()
	{
		return ownerChanges.load(std::memory_order_acquire);
	}

	namespace
	{
		/** @brief How deep the calling thread is in Run (InRun). */
		thread_local std::uint32_t t_runDepth = 0;

		/** @brief Counts the calling thread into Run for the scope. */
		struct RunDepthScope
		{
			RunDepthScope() { ++t_runDepth; }
			~RunDepthScope() { --t_runDepth; }
			RunDepthScope(const RunDepthScope&) = delete;
			RunDepthScope& operator=(const RunDepthScope&) = delete;
		};
	}

	bool InRun()
	{
		return t_runDepth != 0;
	}

	namespace
	{
		/** @brief The graph failed and stopped (a_name's epoch said so): every client returns to its D3D11 path. */
		void StopAll(State& a_state, const char* a_name)
		{
			logger::error("[ORG] frame anchors: the {} epoch failed; {} clients return to D3D11", a_name, a_state.clients.size());
			// What the stages staged is dropped uncommitted.
			for (std::uint32_t d = 0; d < kDeclared; ++d) {
				a_state.stagedFrame[d] = 0;
				a_state.stagedExecuted[d].clear();
			}
			a_state.stagesOpen.store(false, std::memory_order_release);
			auto failed = std::move(a_state.clients);
			a_state.clients.clear();
			a_state.retired.clear();
			Publish(a_state);
			for (const auto& client : failed)
				if (client.desc->onFailed)
					client.desc->onFailed();
		}

		/** @brief A lasting copy of a_text, for the census's causes (which keep pointers). Render owner. */
		const char* Interned(std::string a_text)
		{
			static std::set<std::string> texts;
			return texts.insert(std::move(a_text)).first->c_str();
		}

		/** @brief The frame sequence census: a run's active clients' ids, comma separated. */
		void NoteSequenceRun(const Point& a_point, bool a_held, const std::vector<RunClient>& a_clients)
		{
			if (!RenderGraph::FrameSequence::On())
				return;
			std::string ids;
			for (const auto& client : a_clients)
				if (client.active)
					ids += (ids.empty() ? "" : ",") + client.desc->id;
			RenderGraph::FrameSequence::NoteRun(kInfo[IndexOf(a_point.anchor)].name, a_held, ids, a_point.input, a_point.output);
		}

	}

	namespace
	{
		/** @brief Whether epoch a_segment, submitted now, ends the start stage (BeforeGpuWork): GPU work that follows it in every world frame. */
		bool EndsStart(Segment a_segment)
		{
			switch (a_segment) {
			case Segment::ShadowView:
			case Segment::SkyOcclusion:
			case Segment::AnchorEngineShadowClear:
			case Segment::ZPrepass:
			case Segment::FirstPersonDepth:
			case Segment::LightCulling:
			case Segment::MainOpaque:
			case Segment::DeclaredPreDepth:
			case Segment::DeclaredPreMain:
				return true;
			default:
				return false;
			}
		}

		/** @brief Whether epoch a_segment, submitted now, ends the pre-main stage (BeforeGpuWork): GPU work that follows it in every world frame. */
		bool EndsPreMain(Segment a_segment)
		{
			switch (a_segment) {
			case Segment::MainOpaque:
			case Segment::DeclaredDeferred:
			case Segment::Forward:
				return true;
			default:
				return false;
			}
		}

		/**
		 * @brief Whether epoch a_segment, submitted now, ends the sun stage (BeforeGpuWork): DCLF's first-person epoch, or the work after it in a
		 * frame without it.
		 */
		bool EndsSun(Segment a_segment)
		{
			switch (a_segment) {
			case Segment::FirstPerson:
			case Segment::AnchorVolumetricLighting:
			case Segment::AnchorPostProcessing:
			case Segment::AnchorUpscale:
			case Segment::AnchorInterface:
			case Segment::AnchorPresent:
			case Segment::DeclaredPost:
				return true;
			default:
				return false;
			}
		}

		/** @brief Whether epoch a_segment, submitted now, ends the world segment (BeforeGpuWork): GPU work that follows it in every frame. */
		bool EndsWorld(Segment a_segment)
		{
			switch (a_segment) {
			case Segment::Forward:
			case Segment::FirstPerson:
			case Segment::AnchorEngineQueries:
			case Segment::AnchorVolumetricLighting:
			case Segment::AnchorPostProcessing:
			case Segment::AnchorUpscale:
			case Segment::AnchorInterface:
			case Segment::AnchorPresent:
			case Segment::DeclaredPost:
				return true;
			default:
				return false;
			}
		}
	}

	void BeforeGpuWork(const char* a_why, std::uint32_t a_segment)
	{
		auto& state = Anchors();
		// The deferred+water stage commits just before the next GPU work that comes after it in the frame is submitted: DCLF's forward epoch
		// in a world frame, else the sun's queries (the end of Main::RenderWorld), the first-person epoch or a later segment. Its
		// commit point.
		// The start and pre-main stages commit the same way, before the next GPU work that follows them in every world frame.
		if (a_segment != UINT32_MAX && state.host && SegmentsOn() && IsRenderOwner()) {
			// Work that ends the world segment ends both too, in a frame whose usual commit point did not come (Drawcall Limit Fix not
			// drawing yet).
			const bool late = EndsWorld(static_cast<Segment>(a_segment)) || a_segment == static_cast<std::uint32_t>(Segment::DeclaredDeferred);
			if (late || EndsStart(static_cast<Segment>(a_segment)))
				(void)TapPoint(static_cast<std::uint32_t>(Segment::DeclaredStart));
			if (late || EndsPreMain(static_cast<Segment>(a_segment)))
				(void)TapPoint(static_cast<std::uint32_t>(Segment::DeclaredPreMain));
		}
		if (a_segment != UINT32_MAX && state.host && SegmentsOn() && EndsWorld(static_cast<Segment>(a_segment)) && IsRenderOwner())
			(void)TapPoint(static_cast<std::uint32_t>(Segment::DeclaredDeferred));
		// The sun stage commits just before DCLF's first-person epoch is submitted, or before later GPU work in a frame without it.
		if (a_segment != UINT32_MAX && state.host && SegmentsOn() && EndsSun(static_cast<Segment>(a_segment)) && IsRenderOwner())
			(void)TapPoint(static_cast<std::uint32_t>(Segment::DeclaredSun));
		// The UI stage commits just before the GPU work that follows the overlay: HDR output, the capture, or the present (Hooks.cpp).
		if (a_segment != UINT32_MAX && state.host && SegmentsOn() && IsRenderOwner() &&
			(a_segment == static_cast<std::uint32_t>(Segment::AnchorHDROutput) || a_segment == static_cast<std::uint32_t>(Segment::AnchorCapture))) {
			(void)TapPoint(static_cast<std::uint32_t>(Segment::DeclaredUi));
			(void)TapPoint(static_cast<std::uint32_t>(Segment::DeclaredMenuUi));
		}
		// A declared segment holding its ticket: other GPU work submitted now runs ahead of the segment's work, which the end tap
		// submits. Counted by why, for the census; its first lines are logged.
		if (state.stagesOpen.load(std::memory_order_acquire) && IsRenderOwner()) {
			auto lock = Guard();
			bool own = false;
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				own = own || (state.stagedFrame[d] && a_segment == static_cast<std::uint32_t>(kDeclaredInfo[d].segment));
			// An anchor's run counted its epoch already (Run): its submission is the same GPU work.
			const bool counted = a_segment != UINT32_MAX && state.insideHeldCounted == a_segment;
			if (counted)
				state.insideHeldCounted = UINT32_MAX;
			if (!own && !counted) {
				const char* why = a_segment != UINT32_MAX ? Interned(fmt::format("{} (epoch {})", a_why, a_segment)) : a_why;
				auto found = std::ranges::find_if(state.insideHeld, [&](const auto& a_entry) { return std::string_view(a_entry.first) == why; });
				if (found == state.insideHeld.end())
					state.insideHeld.emplace_back(why, 1);
				else
					++found->second;
				if (++state.insideHeldLines <= 32) {
					std::string held;
					for (std::uint32_t d = 0; d < kDeclared; ++d)
						if (state.stagedFrame[d])
							held += fmt::format("{}{}", held.empty() ? "" : ", ", kDeclaredInfo[d].names.label);
					logger::warn("[ORG] frame segments: {} was submitted between the first tap and the commit of the {}; it runs ahead of the stage's work (frame {})", why, held,
						RenderGraph::Frame::Driver::Current().frame);
				}
			}
		}
	}


	namespace
	{
		using RenderGraph::Frame::SegmentFields;
		using RenderGraph::Frame::SegmentId;
		namespace Driver = RenderGraph::Frame::Driver;

		/** @brief A declared segment's fields for a frame: its built graph, the host's versions, and its clients' declared predicates. */
		SegmentFields DescribeDeclaredFrom(State& a_state, std::uint32_t a_declared, bool a_declaredByKind, const std::shared_ptr<const Snapshot>& a_built,
			const std::shared_ptr<std::vector<std::weak_ptr<org::RenderPass>>>& a_passes, std::uint64_t a_graph, const BuildKey* a_key, bool a_keyed);

		/**
		 * @brief A declared segment's fields for a frame, against its attached build. a_keyed: only what the build's key has present (a pass it
		 * left out is absent); else the frame's own description, which may want more.
		 */
		SegmentFields DescribeDeclared(State& a_state, std::uint32_t a_declared, bool a_declaredByKind, bool a_keyed = true)
		{
			if (!a_declaredByKind || !a_state.host)
				return DescribeDeclaredFrom(a_state, a_declared, a_declaredByKind, nullptr, nullptr, 0, nullptr, false);
			const auto key = a_state.declaredKeys[a_declared].load(std::memory_order_acquire);
			return DescribeDeclaredFrom(a_state, a_declared, a_declaredByKind, a_state.declaredSnapshots[a_declared].load(std::memory_order_acquire),
				a_state.declaredPasses[a_declared].load(std::memory_order_acquire), a_state.host->BuildGeneration(EpochOfDeclared(a_declared)), key.get(), a_keyed);
		}

		/**
		 * @brief A declared segment's fields for a frame against a build (a_built its snapshot, a_passes its passes, a_graph its generation,
		 * a_key its key). a_keyed: the description is cut to what the key has present.
		 */
		SegmentFields DescribeDeclaredFrom(State& a_state, std::uint32_t a_declared, bool a_declaredByKind, const std::shared_ptr<const Snapshot>& a_built,
			const std::shared_ptr<std::vector<std::weak_ptr<org::RenderPass>>>& a_passes, std::uint64_t a_graph, const BuildKey* a_key, bool a_keyed)
		{
			SegmentFields fields;
			fields.declared = a_declaredByKind;
			if (!a_declaredByKind || !a_state.host)
				return fields;
			const auto& built = a_built;
			if (!built)
				return fields;  // graph 0: not built yet
			fields.graph = a_graph;
			fields.versions = a_state.declaredRecordings[a_declared].versionGeneration;
			const auto& clients = SegmentClients(*built, a_declared);
			// A stage whose graph holds no client and no module step has nothing to record (the UI stage before the overlay registers):
			// it is absent from the frame, as before its first build.
			bool foreignStep = false;
			for (std::uint32_t k = 0; k < kDeclaredInfo[a_declared].stepCount; ++k)
				foreignStep = foreignStep || kDeclaredInfo[a_declared].steps[k].foreign != Segment::Count;
			if (clients.empty() && !foreignStep) {
				fields.graph = 0;
				return fields;
			}
			fields.clients = static_cast<std::uint32_t>(clients.size());
			for (const auto& client : clients)
				if (client->rows) {
					std::array<std::uint64_t, 4> rows{};
					a_state.describingAgainst = a_key;
					a_state.describingClient = client.get();
					client->rows(rows);
					a_state.describingAgainst = nullptr;
					a_state.describingClient = nullptr;
					for (std::size_t w = 0; w < rows.size(); ++w)
						fields.rows[w] |= rows[w];
				}
			const auto& info = kDeclaredInfo[a_declared];
			for (std::size_t i = 0; i < clients.size() && i < RenderGraph::Frame::kSegmentClients; ++i) {
				if (!clients[i]->declared || clients[i]->declared())
					fields.clientsOn |= 1u << i;
				if (clients[i]->maxInstances > 1)
					fields.instances[i] = static_cast<std::uint8_t>(std::min<std::uint32_t>(
						{ clients[i]->instances ? clients[i]->instances() : 1u, clients[i]->maxInstances, 255u }));
			}
			for (std::uint32_t k = 0; k < info.stepCount; ++k)
				if (info.steps[k].foreign != Segment::Count)
					if (const auto foreign = ForeignOf(info.steps[k].foreign); foreign && (!foreign->declared || foreign->declared()))
						fields.foreignOn |= 1u << k;
			// What the build's key left out is absent: the description has it off.
			if (a_keyed && a_key && !a_key->all) {
				for (std::size_t i = 0; i < clients.size(); ++i) {
					const auto [present, rows] = KeyPart(a_key, *clients[i]);
					const auto own = RowsOf(*clients[i]);
					for (std::size_t w = 0; w < 4; ++w)
						fields.rows[w] &= ~(own[w] & ~rows[w]);
					if (!present && i < RenderGraph::Frame::kSegmentClients) {
						fields.clientsOn &= ~(1u << i);
						fields.instances[i] = 0;
					}
				}
				fields.foreignOn &= KeyForeign(a_key, ~0u);
			}
			// The passes recorded ahead name what their recordings depend on beyond their bindings: a kept recording binds those
			// revisions, so they are part of the description. A pass that cannot tell is logged once a build.
			if (const auto& passes = a_passes) {
				std::uint64_t hash = 1469598103934665603ull;
				std::uint32_t untold = 0;
				for (const auto& weak : *passes) {
					const auto pass = weak.lock();
					std::uint64_t revision = 0;
					if (!pass || !pass->RevisionHash(revision)) {
						++untold;
						continue;
					}
					hash = (hash ^ revision) * 1099511628211ull;
				}
				fields.passes = hash;
				static std::array<std::uint64_t, kDeclared> logged{};
				if (untold && logged[a_declared] != fields.graph) {
					logged[a_declared] = fields.graph;
					logger::warn("[ORG] frame segments: {} of the {}'s passes recorded ahead give no revision; a change they depend on is not seen", untold,
						info.names.label);
				}
			}
			return fields;
		}

		/**
		 * @brief Stops the game: a declared segment's recording failed. The frame cannot be drawn without it, and drawing it another way
		 * would hide the defect. In every build.
		 */
		[[noreturn]] void FailRecording(std::uint32_t a_declared, const SegmentFields& a_fields, std::uint32_t a_slot, std::string_view a_cause)
		{
			const auto message = fmt::format(
				"Community Shaders native mode: the {} could not be recorded, so the frame cannot be drawn. Description: graph build {}, "
				"version generation {}, clients on {:#x} of {}, module steps on {:#x}, pass revisions {:#x}, frame slot {}. Cause: {}",
				kDeclaredInfo[a_declared].names.label, a_fields.graph, a_fields.versions, a_fields.clientsOn, a_fields.clients, a_fields.foreignOn,
				a_fields.passes, a_slot, a_cause);
			logger::critical("[ORG] frame segments: {}", message);
			stl::report_and_fail(message);
		}

		/**
		 * @brief The recording of a declared segment for a_fields: the one kept for them, or a new request to the host's thread (logged
		 * with the fields that changed since the last request). A kept one that a build dropped is asked for again. A kept one that
		 * failed stops the game (FailRecording). Only BeginFrame asks (Schedule); a request at any other time is counted as a mid-frame
		 * re-record. Never waits. Render owner.
		 */
		std::shared_ptr<SegmentRecording> RequestDeclared(State& a_state, std::uint32_t a_declared, const SegmentFields& a_fields, std::uint32_t a_slot,
			std::uint64_t a_frame, std::string_view a_why)
		{
			auto& recordings = a_state.declaredRecordings[a_declared];
			const auto hash = RenderGraph::Frame::FieldsHash(a_fields);
			std::string reason;
			auto entry = std::make_shared<SegmentRecording>();
			{
				std::unique_lock lock(recordings.mutex);
				for (auto it = recordings.entries.begin(); it != recordings.entries.end(); ++it) {
					auto& kept = **it;
					if (kept.hash != hash || kept.slot != a_slot || !(kept.fields == a_fields))
						continue;
					if (kept.done && !kept.recording) {
						if (!kept.dropped)
							FailRecording(a_declared, kept.fields, kept.slot, kept.error.empty() ? std::string("no recording") : kept.error);
						recordings.entries.erase(it);
						break;
					}
					kept.lastFrame = a_frame;
					return *it;
				}
				reason = recordings.any ? RenderGraph::Frame::DiffFields(recordings.last, a_fields) : std::string("the first description");
				if (reason.empty()) {
					// The description recorded for another slot already, or a recording of it that a build dropped.
					const bool other = std::ranges::any_of(recordings.entries, [&](const auto& a_entry) { return a_entry->hash == hash && a_entry->slot != a_slot; });
					reason = other ? fmt::format("the same description, for slot {}", a_slot) : std::string("the same description, a build dropped its last recording");
				}
				recordings.last = a_fields;
				recordings.any = true;
				entry->hash = hash;
				entry->slot = a_slot;
				entry->fields = a_fields;
				entry->lastFrame = a_frame;
				// Recordings of an older build are never used again, and each keeps that build's graph alive with its private alias
				// pools (tens of MB a build). The staged build's are kept: it switches in only once every slot's recording is ready, and a
				// description that alternates by frame (TAA's history rows) asks again each frame. Dropping them there kept the staged
				// build from ever switching in.
				const auto staged = a_state.host->StagedGeneration(EpochOfDeclared(a_declared));
				std::erase_if(recordings.entries, [&](const auto& a_kept) {
					return a_kept->done && a_kept->recording && !a_state.host->EpochRecordingCurrent(*a_kept->recording) &&
					       (staged == 0 || a_kept->recording->buildGeneration != staged);
				});
				// The least recently used of the finished ones goes when the list is full.
				while (recordings.entries.size() >= SegmentRecordings::kKeep) {
					auto oldest = recordings.entries.end();
					for (auto it = recordings.entries.begin(); it != recordings.entries.end(); ++it)
						if ((*it)->done && (oldest == recordings.entries.end() || (*it)->lastFrame < (*oldest)->lastFrame))
							oldest = it;
					if (oldest == recordings.entries.end())
						break;
					recordings.entries.erase(oldest);
				}
				recordings.entries.push_back(entry);
			}
			Driver::NoteRecording(static_cast<SegmentId>(a_declared), a_frame, fmt::format("{}{}", reason, a_why));
			if (!a_state.inBegin)
				Driver::NoteLateRequest(static_cast<SegmentId>(a_declared));
			auto data = std::make_shared<SegmentHostData>();
			data->segment = a_declared;
			data->fields = a_fields;
			const bool posted = a_state.host->RequestEpochRecording(EpochOfDeclared(a_declared), std::move(data),
				[&recordings, entry](std::shared_ptr<const org::GraphHost::EpochRecording> a_recording, std::exception_ptr a_error) {
					std::string error;
					bool dropped = false;
					if (a_error) {
						try {
							std::rethrow_exception(a_error);
						} catch (const org::GraphHost::EpochRecordingDropped& e) {
							error = e.what();
							dropped = true;
						} catch (const std::exception& e) {
							error = e.what();
						} catch (...) {
							error = "unknown error";
						}
					}
					if (!a_recording && !a_error)
						error = "the host's thread completed the request with neither a recording nor an error";
					{
						const std::lock_guard lock(recordings.mutex);
						entry->recording = std::move(a_recording);
						entry->error = std::move(error);
						entry->dropped = dropped;
						entry->done = true;
					}
				},
				a_slot, a_fields.graph);
			if (!posted) {
				const std::lock_guard lock(recordings.mutex);
				entry->error = "the host takes no recording requests (async epochs are off)";
				entry->done = true;
			}
			return entry;
		}

		/**
		 * @brief A declared segment's schedule for this frame (BeginFrame). The frame slot keeps its schedule until the recording of the
		 * frame's own description for that slot is ready; then it switches, here, before any of the frame's work. The recordings of a
		 * new description are asked for here (this slot's and the next slots'), on the host's thread. Nothing waits. Returns the fields
		 * the frame runs on: the schedule's, or the frame's own description with graph 0 when the slot has no schedule on the attached
		 * build (the segment is absent this frame: its taps draw nothing). Render owner.
		 */
		SegmentFields Schedule(State& a_state, std::uint32_t a_declared, bool a_world, const RenderGraph::Frame::Description& a_frame)
		{
			const auto segment = static_cast<SegmentId>(a_declared);
			auto& host = *a_state.host;
			const std::uint32_t slot = a_frame.slot;
			auto& active = a_state.schedule[a_declared][slot];
			auto& recordings = a_state.declaredRecordings[a_declared];
			// A schedule of an older build is gone. Builds attach only at a frame boundary, before this.
			if (active.recording && !host.EpochRecordingCurrent(*active.recording))
				active = {};
			// A schedule whose recording binds a version a slot replaced since: the segment's version generation moves, so the frame's
			// description is a new one, recorded for the new version. Until that recording is ready the slot keeps its schedule, which
			// binds (and holds) the old version: the stage lags a frame or two, it is never blank.
			a_state.frameVersionsCurrent[a_declared] = true;
			if (active.recording && !host.EpochRecordingVersionsCurrent(*active.recording, slot)) {
				if (active.fields.versions == recordings.versionGeneration)
					++recordings.versionGeneration;
				a_state.frameVersionsCurrent[a_declared] = false;
			}
			// A staged build (docs/development/org-native-frame-graph.md, "Builds switch in with their recordings"): recorded on the host's
			// thread for every frame slot, for the frame's description of it, and switched in whole here, at a frame boundary, once every
			// slot's recording is ready. Until then every slot keeps the attached build's schedule.
			const auto epoch = EpochOfDeclared(a_declared);
			// Recorded and switched in frames of any kind, so a build made during a load or a menu is in place for the next world frame.
			const std::uint64_t stagedGeneration = host.StagedGeneration(epoch);
			if (stagedGeneration)
				if (const auto stagedBuilt = a_state.stagedSnapshots[a_declared].load(std::memory_order_acquire)) {
					const auto stagedKey = a_state.stagedKeys[a_declared].load(std::memory_order_acquire);
					const auto staged = DescribeDeclaredFrom(a_state, a_declared, true, stagedBuilt, a_state.stagedPasses[a_declared].load(std::memory_order_acquire),
						stagedGeneration, stagedKey.get(), true);
					std::array<std::shared_ptr<const org::GraphHost::EpochRecording>, RenderGraphRuntime::kGameFramesInFlight> ready{};
					bool all = true;
					// A staged build with nothing to record (no client) switches in at once; the stage is absent on it.
					for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight && staged.graph; ++s) {
						const auto entry = RequestDeclared(a_state, a_declared, staged, s, a_frame.frame, " (staged build)");
						const std::lock_guard lock(recordings.mutex);
						if (entry->done && entry->recording)
							ready[s] = entry->recording;
						else
							all = false;
					}
					if (a_state.stagedSince[a_declared] == 0)
						a_state.stagedSince[a_declared] = a_frame.frame;
					if (all && RenderGraph::Builds::SwitchStaged(epoch)) {
						const auto built = a_state.declaredSnapshots[a_declared].load(std::memory_order_acquire);
						// A build asked for by a new key: how many frames its passes took from the Begin that wanted them to this one.
						std::string keyNote;
						if (auto& use = a_state.keyUse[a_declared]; stagedKey && !stagedKey->all && stagedKey->serial >= use.requestedSerial && use.switchedSerial < use.requestedSerial) {
							keyNote = fmt::format("; its key was asked for at frame {}, {} frames before", use.requestedFrame, a_frame.frame - use.requestedFrame);
							use.switchedSerial = use.requestedSerial;
						}
						const auto what = fmt::format("build {} switched in whole with its recordings for every frame slot{}{}", stagedGeneration,
							active.recording ? fmt::format(" ({})", RenderGraph::Frame::DiffFields(active.fields, staged)) : std::string(), keyNote);
						for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight; ++s)
							a_state.schedule[a_declared][s] = staged.graph ? State::Scheduled{ staged, ready[s], built } : State::Scheduled{};
						Driver::NoteSwitch(segment, a_frame.frame, slot, a_frame.frame - a_state.stagedSince[a_declared], what);
						Driver::NoteBuildSwitch(segment);
						a_state.stagedSince[a_declared] = 0;
						a_state.stagedSnapshots[a_declared].store(nullptr, std::memory_order_release);
						a_state.stagedPasses[a_declared].store(nullptr, std::memory_order_release);
						a_state.stagedKeys[a_declared].store(nullptr, std::memory_order_release);
					}
				}
			const bool stillStaged = a_world && host.StagedGeneration(epoch) != 0;
			// The frame's description cut to what the attached build has present, and the frame's own: the taps take work the build left
			// out as lagged work (the key asks for its build at this Begin).
			SegmentFields wanted = DescribeDeclared(a_state, a_declared, a_world);
			a_state.frameWanted[a_declared] = DescribeDeclared(a_state, a_declared, a_world, false);
			const bool keyLags = !(a_state.frameWanted[a_declared] == wanted);
			// A stage outside its kinds' frames: the frame does not declare it, but its schedules are recorded ahead, so the frame-kind switch to its
			// kinds finds a schedule in every frame slot and no frame has the stage absent. A UI stage (KeptAhead) keeps every slot on the
			// description its frames would have (no display is left out at the switch); any other stage only gets a first schedule for a slot
			// that has none (its first frames lag at worst, they are never blank).
			if (!a_world && host.StagedGeneration(epoch) == 0) {
				const SegmentFields ahead = DescribeDeclared(a_state, a_declared, true);
				for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight && ahead.graph; ++s) {
					auto& kept = a_state.schedule[a_declared][s];
					if (!KeptAhead(a_declared) && kept.recording && host.EpochRecordingCurrent(*kept.recording))
						continue;
					// The description is this frame's: its parity fields (the slot-parity variants) fit only the slots of this frame's parity.
					if (s % 2 != a_frame.parity)
						continue;
					if (kept.recording && kept.fields == ahead && host.EpochRecordingCurrent(*kept.recording)) {
						// A version replaced since: the next frame's description names the new one, and its recording replaces this.
						if (!host.EpochRecordingVersionsCurrent(*kept.recording, s) && kept.fields.versions == recordings.versionGeneration)
							++recordings.versionGeneration;
						continue;
					}
					const auto entry = RequestDeclared(a_state, a_declared, ahead, s, a_frame.frame, " (ahead, for the next frame of its kinds)");
					std::shared_ptr<const org::GraphHost::EpochRecording> ready;
					{
						const std::lock_guard lock(recordings.mutex);
						if (entry->done)
							ready = entry->recording;
					}
					if (ready && host.EpochRecordingCurrent(*ready) && host.EpochRecordingVersionsCurrent(*ready, s))
						kept = { ahead, std::move(ready), a_state.declaredSnapshots[a_declared].load(std::memory_order_acquire) };
					else if (ready && host.EpochRecordingCurrent(*ready) && ahead.versions == recordings.versionGeneration)
						++recordings.versionGeneration;
				}
			}
			if (!wanted.declared)
				return wanted;
			if (!wanted.graph) {
				Driver::NoteScheduled(segment, Driver::Scheduled::Absent);
				return wanted;
			}
			// While a build is staged the attached graph records nothing new (its bindings are frozen): the slots keep what they have.
			if (!(active.recording && active.fields == wanted) && !stillStaged) {
				std::shared_ptr<SegmentRecording> entry;
				for (std::uint32_t ahead = 0; ahead < RenderGraphRuntime::kGameFramesInFlight; ++ahead) {
					auto requested = RequestDeclared(a_state, a_declared, wanted, (slot + ahead) % RenderGraphRuntime::kGameFramesInFlight, a_frame.frame, "");
					if (ahead == 0)
						entry = std::move(requested);
				}
				const auto hash = RenderGraph::Frame::FieldsHash(wanted);
				if (a_state.waitHash[a_declared] != hash) {
					a_state.waitHash[a_declared] = hash;
					a_state.waitFrame[a_declared] = a_frame.frame;
				}
				std::shared_ptr<const org::GraphHost::EpochRecording> ready;
				{
					const std::lock_guard lock(recordings.mutex);
					if (entry->done)
						ready = entry->recording;
				}
				if (ready && host.EpochRecordingCurrent(*ready) && host.EpochRecordingVersionsCurrent(*ready, slot)) {
					const auto what = active.recording ? RenderGraph::Frame::DiffFields(active.fields, wanted) :
					                                     fmt::format("the slot's first schedule on build {}", wanted.graph);
					active = { wanted, std::move(ready), a_state.declaredSnapshots[a_declared].load(std::memory_order_acquire) };
					Driver::NoteSwitch(segment, a_frame.frame, slot, a_frame.frame - a_state.waitFrame[a_declared], what);
					// A UI stage: the other slots switch at this boundary too when their recordings of the description are ready, so the next
					// frames do not lag on an older schedule. Not for other stages: a description that alternates by frame (the post rows'
					// ping-pong) keeps one schedule per slot.
					for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight && KeptAhead(a_declared); ++s) {
						auto& other = a_state.schedule[a_declared][s];
						if (s == slot || (other.recording && other.fields == wanted))
							continue;
						const auto otherEntry = RequestDeclared(a_state, a_declared, wanted, s, a_frame.frame, "");
						std::shared_ptr<const org::GraphHost::EpochRecording> otherReady;
						{
							const std::lock_guard lock(recordings.mutex);
							if (otherEntry->done)
								otherReady = otherEntry->recording;
						}
						if (otherReady && host.EpochRecordingCurrent(*otherReady) && host.EpochRecordingVersionsCurrent(*otherReady, s))
							other = { wanted, std::move(otherReady), active.built };
					}
				} else if (ready && host.EpochRecordingCurrent(*ready)) {
					// Recorded with a version a slot replaced while it was made: the next frame's description names the new version.
					++recordings.versionGeneration;
				}
			}
			if (!active.recording) {
				Driver::NoteScheduled(segment, Driver::Scheduled::Absent);
				wanted.graph = 0;
				return wanted;
			}
			Driver::NoteScheduled(segment, active.fields == wanted && !keyLags ? Driver::Scheduled::Current : Driver::Scheduled::Lagging);
			a_state.frameRecording[a_declared] = active.recording;
			a_state.frameBuilt[a_declared] = active.built;
			return active.fields;
		}

		/** @brief CS_ORG_ABSENT_PASSES=0 keeps every pass in the stages' builds, off passes skipped and folded (A/B runs). */
		bool AbsentPassesOn()
		{
			static const bool on = !Switches::Equals("CS_ORG_ABSENT_PASSES", "0");
			return on;
		}

		/** @brief Seconds of a stage's own clock that a pass no frame wants stays present. */
		constexpr double kKeyHold = 10.0;

		/**
		 * @brief A stage's key at Begin (docs/development/org-native-frame-graph.md, "Absent passes"). The stage's passes are described as
		 * a key (DescribingKey: every variant a frame may pick, both kind groups, the passes always present) against the clients registered
		 * now, and each wanted one is stamped on the stage's clock. The key keeps what was wanted within kKeyHold. A build is asked for when
		 * the key wants a pass the stage's newest build (staged, else attached) left out, and, to drop unused passes, only at a load, a
		 * frame-kind switch, or when that build's key was provisional (the first key, a client registered since). Otherwise the key is only
		 * stored for the stage's next build, whatever asks for it. A steady scene asks for nothing. Render owner, inside BeginFrame.
		 */
		void UpdateKey(State& a_state, std::uint32_t a_declared, bool a_inRecipe, const RenderGraph::Frame::Description& a_frame, bool a_kindSwitch)
		{
			if (!AbsentPassesOn() || !a_state.host)
				return;
			auto& use = a_state.keyUse[a_declared];
			const auto snapshot = a_state.snapshot.load(std::memory_order_acquire);
			if (!snapshot)
				return;
			const auto& info = kDeclaredInfo[a_declared];
			const auto& clients = SegmentClients(*snapshot, a_declared);
			std::erase_if(use.clients, [&](const KeyUse::Client& a_client) { return std::ranges::find(clients, a_client.desc) == clients.end(); });
			const bool load = Driver::LoadStartedThisFrame();
			if (a_inRecipe) {
				const auto now = std::chrono::steady_clock::now();
				if (use.last != std::chrono::steady_clock::time_point{})
					use.clock += (std::min)(0.1, std::chrono::duration<double>(now - use.last).count());
				use.last = now;
				a_state.describingKey = true;
				const SegmentFields wanted = DescribeDeclaredFrom(a_state, a_declared, true, snapshot, nullptr, 1, nullptr, false);
				const SegmentFields other{};
				a_state.describingKey = false;
				if (wanted.graph) {
					for (std::size_t i = 0; i < clients.size(); ++i) {
						auto found = std::ranges::find_if(use.clients, [&](const KeyUse::Client& a_client) { return a_client.desc == clients[i]; });
						if (found == use.clients.end()) {
							use.clients.emplace_back().desc = clients[i];
							found = std::prev(use.clients.end());
						}
						const bool on = i >= RenderGraph::Frame::kSegmentClients || ((wanted.clientsOn | other.clientsOn) >> i & 1u);
						if (!on)
							continue;
						found->used = use.clock;
						const auto own = RowsOf(*clients[i]);
						for (std::uint32_t r = 0; r < 256; ++r)
							if ((own[r / 64] & (wanted.rows[r / 64] | other.rows[r / 64])) >> (r % 64) & 1u)
								found->rows[r] = use.clock;
					}
					for (std::uint32_t k = 0; k < info.stepCount && k < 32; ++k)
						if ((wanted.foreignOn | other.foreignOn) >> k & 1u)
							use.foreign[k] = use.clock;
				}
			} else {
				use.last = {};
				// Outside the stage's frames a key changes only at a load or a kind switch, and only once the stage was described.
				if ((!load && !a_kindSwitch) || use.clients.empty())
					return;
			}
			// The key: what was wanted within the hold. A client never described yet is not listed (present whole).
			auto keep = std::make_shared<BuildKey>();
			keep->all = false;
			keep->frame = a_frame.frame;
			bool any = false;
			for (const auto& client : clients) {
				const auto found = std::ranges::find_if(use.clients, [&](const KeyUse::Client& a_client) { return a_client.desc == client; });
				if (found == use.clients.end()) {
					any = true;
					continue;
				}
				BuildKey::Client part{ .desc = client, .present = use.clock - found->used <= kKeyHold };
				if (part.present) {
					const auto own = RowsOf(*client);
					for (std::uint32_t r = 0; r < 256; ++r)
						if ((own[r / 64] >> (r % 64) & 1u) && use.clock - found->rows[r] <= kKeyHold)
							part.rows[r / 64] |= 1ull << (r % 64);
					any = true;
				}
				keep->clients.push_back(part);
			}
			for (std::uint32_t k = 0; k < info.stepCount && k < 32; ++k)
				if (info.steps[k].foreign != Segment::Count && use.clock - use.foreign[k] <= kKeyHold) {
					keep->foreign |= 1u << k;
					any = true;
				}
			// A stage is never left with nothing to record: its first client stays.
			if (!any && !keep->clients.empty()) {
				keep->clients.front().present = true;
				keep->clients.front().rows = RowsOf(*keep->clients.front().desc);
			}
			const auto epoch = EpochOfDeclared(a_declared);
			const bool hasStaged = a_state.host->StagedGeneration(epoch) != 0 && a_state.stagedSnapshots[a_declared].load(std::memory_order_acquire);
			const auto built = hasStaged ? a_state.stagedKeys[a_declared].load(std::memory_order_acquire) : a_state.declaredKeys[a_declared].load(std::memory_order_acquire);
			const auto next = a_state.nextKeys[a_declared].load(std::memory_order_acquire);
			// The stage's module steps, a bit each.
			std::uint32_t steps = 0;
			for (std::uint32_t k = 0; k < info.stepCount && k < 32; ++k)
				if (info.steps[k].foreign != Segment::Count)
					steps |= 1u << k;
			const auto equal = [&](const BuildKey* a_left, const BuildKey* a_right) {
				return a_left && a_right && KeyCovers(a_left, a_right, clients, steps) && KeyCovers(a_right, a_left, clients, steps);
			};
			const bool covered = KeyCovers(built.get(), keep.get(), clients, steps);
			const bool stale = !KeyCovers(keep.get(), built.get(), clients, steps);
			// A build asked for with a key not in place yet (staged or attached) is on its way. One that never came (a failed build) is asked
			// for again after a while.
			constexpr std::uint64_t kRequestFrames = 600;
			const bool inFlight = use.requestedSerial && (!built || built->serial < use.requestedSerial) && a_frame.frame - use.requestedFrame < kRequestFrames;
			const char* why = nullptr;
			if (!covered) {
				if (!(inFlight && next && KeyCovers(next.get(), keep.get(), clients, steps)))
					why = "a frame wants passes the build left out";
			} else if (stale) {
				const bool provisional = KeyProvisional(built.get(), clients);
				if ((provisional || load || a_kindSwitch) && !(inFlight && equal(next.get(), keep.get())))
					why = provisional ? (!built || built->all ? "the stage's first key" : "a client registered since the build") :
					      load        ? "unused passes leave at a load" :
					                    "unused passes leave at a frame-kind switch";
			}
			if (why) {
				keep->serial = ++a_state.keySerial;
				const auto text = DiffKeys(built.get(), keep.get(), clients, steps);
				use.requestedSerial = keep->serial;
				use.requestedFrame = a_frame.frame;
				a_state.nextKeys[a_declared].store(keep, std::memory_order_release);
				logger::info("[ORG] absent passes: the {} asks for a build at frame {} ({}): {}", info.names.label, a_frame.frame, why, text);
				RenderGraph::Builds::Request(epoch, fmt::format("frame anchors: the {}'s key: {}", info.names.label, why));
				Driver::NoteKeyBuild(static_cast<SegmentId>(a_declared));
			} else if (!equal(next.get(), keep.get())) {
				// Passes unused for the hold leave with the stage's next build, whatever asks for it.
				keep->serial = ++a_state.keySerial;
				a_state.nextKeys[a_declared].store(keep, std::memory_order_release);
			}
		}

		/** @brief Whether any stage holds staged values not committed yet. Render owner. */
		void UpdateStagesOpen(State& a_state)
		{
			a_state.stagesOpen.store(std::ranges::any_of(a_state.stagedFrame, [](std::uint64_t a_frame) { return a_frame != 0; }), std::memory_order_release);
		}

		/** @brief Drops what a stage's taps staged this frame without committing it. Render owner. */
		void DropStaged(State& a_state, std::uint32_t a_declared)
		{
			a_state.stagedFrame[a_declared] = 0;
			a_state.stagedExecuted[a_declared].clear();
			UpdateStagesOpen(a_state);
		}

		/** @brief Every run gate a stage's graph holds: its clients' and those of registrations it still holds after they left. */
		template <class F>
		void ForEachStageGate(const State& a_state, std::uint32_t a_declared, const Snapshot& a_built, F&& a_do)
		{
			const auto anchors = StepAnchors(a_declared);
			for (const auto& client : SegmentClients(a_built, a_declared))
				for (const auto& gate : client->gates)
					if (gate)
						a_do(*gate);
			for (const auto& retired : a_state.retired)
				if ((retired.mask & anchors) && a_built.serial < retired.serial)
					for (const auto& gate : retired.gates)
						if (gate)
							a_do(*gate);
		}

		/**
		 * @brief Begin: a present stage's frame inputs for this frame slot are made empty. Its staging run's latch regions are zeroed and
		 * every gate of its graph is closed there, so a tap's commit writes only what this frame staged. The GPU never reads the staging
		 * run, so this waits for nothing. Render owner.
		 */
		void OpenInputs(State& a_state, std::uint32_t a_declared, std::uint32_t a_slot)
		{
			auto& host = *a_state.host;
			const auto& built = a_state.frameBuilt[a_declared];
			a_state.stepTaps[a_declared].fill(0);
			a_state.stagedExecuted[a_declared].clear();
			if (!built)
				return;
			ZoneScopedN("CS.Stage.OpenInputs");
			const auto staging = host.EpochStagingRun(EpochOfDeclared(a_declared), a_slot);
			a_state.stagingRun[a_declared] = staging;
			// Every latch region and every gate's record of the staging run cleared: a gate its client does not list (ClientDesc::gates)
			// reads closed too unless a tap opens it.
			org::LatchBlock::ZeroStableRun(staging.lease);
			RenderGraph::RunGate::ClearOpenAll(staging);
			ForEachStageGate(a_state, a_declared, *built, [&](const RenderGraph::RunGate& a_gate) { a_gate.Close(staging); });
		}

		/**
		 * @brief A stage's one commit, at its commit point: the values its taps staged this frame are copied into the frame slot's copy,
		 * and its one ticket is submitted with the recording of the frame's schedule. Then the executed callbacks of every client that
		 * staged. Render owner, under the anchors' mutex.
		 */
		RunResult SubmitSegment(State& a_state, const Point& a_point, std::uint32_t a_declared)
		{
			const auto segment = static_cast<SegmentId>(a_declared);
			const auto& frame = Driver::Current();
			auto& host = *a_state.host;
			auto& runtime = RenderGraphRuntime::Get();
			const std::uint32_t epoch = EpochOfDeclared(a_declared);
			// The frame runs on the schedule it took at Begin: nothing is described, recorded or waited for here.
			const auto recording = a_state.frameRecording[a_declared];
			const auto built = a_state.frameBuilt[a_declared];
			const auto staging = a_state.stagingRun[a_declared];
			auto executed = std::exchange(a_state.stagedExecuted[a_declared], {});
			const auto mismatch = [&](Driver::TapError a_error, std::string_view a_what) {
				Driver::NoteTapError(a_error, segment, a_what);
				DropStaged(a_state, a_declared);
				return RunResult{ RunStatus::Idle };
			};
			if (!recording || !built)
				return mismatch(Driver::TapError::NotDeclared, "its commit came in a frame without a schedule for it");
			if (!host.EpochRecordingCurrent(*recording))
				return mismatch(Driver::TapError::LostToBuild, "a build of its graph attached after the frame's Begin; builds attach only at a frame boundary");
			if (a_state.frameVersionsCurrent[a_declared] && !host.EpochRecordingVersionsCurrent(*recording, frame.slot))
				return mismatch(Driver::TapError::StaleRecording,
					"a slot of its graph published a new version after the frame's Begin; versions are published only at a frame boundary");
			const auto expected = host.EpochCopyRun(epoch, frame.slot);
			RenderGraphRuntime::EpochResult result = RenderGraphRuntime::EpochResult::Stopped;
			try {
				host.SetEpochCopy(epoch, frame.slot);
				RenderGraphRuntime::EpochBodyScope body(kDeclaredInfo[a_declared].segment);
				result = runtime.TryExecuteEpoch(kDeclaredInfo[a_declared].segment, [&](org::RenderGraph&) {
					// The copy's last run is done once its ticket is taken: the staged values become its run's.
					const auto run = host.CurrentRun();
					if (!(run == expected))
						Driver::NoteTapError(Driver::TapError::SlotMismatch, segment,
							fmt::format("the run is lease {} copy {}, the slot's copy is lease {} copy {}", run.lease, run.copy, expected.lease, expected.copy));
					RenderGraph::RunShapes::BeginRun();
					{
						ZoneScopedN("CS.Stage.CopyLatch");
						org::LatchBlock::CopyStableRun(staging.lease, run.lease);
					}
					{
						ZoneScopedN("CS.Stage.CopyGates");
						RenderGraph::RunGate::CopyOpenAll(staging, run);
					}
					{
						ZoneScopedN("CS.Stage.MoveSlots");
						RenderGraph::RunSlotsRegistry::MoveAll(staging, run);
					}
					host.UseEpochRecording(recording);
				});
			} catch (const std::exception& e) {
				logger::error("[ORG] frame segments: the {} could not be submitted: {}", kDeclaredInfo[a_declared].names.label, e.what());
				result = RenderGraphRuntime::EpochResult::Stopped;
			}
			// Submitted or not, the frame's inputs of the stage are spent.
			a_state.stagedFrame[a_declared] = 0;
			UpdateStagesOpen(a_state);
			if (result == RenderGraphRuntime::EpochResult::Skipped)
				return { RunStatus::Refused };
			if (result == RenderGraphRuntime::EpochResult::EpochFailed)
				return { RunStatus::Failed };
			if (result != RenderGraphRuntime::EpochResult::Submitted) {
				StopAll(a_state, kDeclaredInfo[a_declared].names.label);
				return { RunStatus::Failed };
			}
			const auto anchors = StepAnchors(a_declared);
			bool late = false;
			for (std::uint32_t i = 0; i < kAnchors; ++i)
				late = late || ((anchors >> i & 1u) && kInfo[i].late);
			if (late && a_state.lateHook)
				a_state.lateHook();
			if (SegmentUsesSharedConstants(*built, a_declared))
				SharedConstants::CheckAfterRun();
			// A point step or a cut ends the segment at no anchor: no run shape.
			if (RenderGraph::RunShapes::On() && a_point.anchor != Anchor::Count) {
				const std::uint32_t index = IndexOf(a_point.anchor);
				RenderGraph::RunShapes::EndRun(index, kInfo[index].name, a_point.input, a_point.output, 0);
			}
			PruneRetired(a_state);
			// One-shot passes in the graph this run submitted have run: the next build leaves them out, and the next frame's description
			// names that build.
			// Only the anchors those passes sit at are built again: the epochs that hold them. Rebuilding every anchor of the stage also
			// rebuilt the per-anchor epochs of its shared anchors, unstaged, which held Drawcall Limit Fix after each load.
			std::uint32_t ranOnce = 0;
			for (auto& client : a_state.clients)
				for (std::size_t i = 0; i < client.desc->passes.size(); ++i)
					if (client.armed[i] && client.armed[i] <= built->serial && SegmentHoldsPass(a_declared, *client.desc, client.desc->passes[i])) {
						client.armed[i] = 0;
						ranOnce |= 1u << IndexOf(client.desc->passes[i].anchor);
					}
			if (ranOnce) {
				Publish(a_state);
				Rebuild(a_state, ranOnce, "one-shot passes ran");
			}
			// Last: an executed callback may register, refresh or rearm. What each staged commit wrote is in the submitted run now.
			for (const auto& [client, point] : executed)
				if (client->executed)
					client->executed(point);
			return { RunStatus::Ran };
		}

		/**
		 * @brief A declared stage's tap: one of its steps' hooks. The tap's clients stage their values: their commits write into the
		 * frame slot's staging run (the frame inputs), with the step's shared constants. No ticket is taken here. The last step's tap is
		 * the stage's one commit (SubmitSegment). A tap the description does not cover is a tap error and stages nothing. a_foreign: a
		 * foreign step's commit (TapForeign), in place of the clients'. Render owner, under the anchors' mutex.
		 */
		RunResult TapDeclared(State& a_state, const Point& a_point, std::uint32_t a_declared, std::uint32_t a_step, const std::vector<RunClient>& a_clients,
			const std::function<void()>* a_foreign = nullptr)
		{
			const auto segment = static_cast<SegmentId>(a_declared);
			const auto& info = kDeclaredInfo[a_declared];
			const auto& step = info.steps[a_step];
			const char* tap = step.anchor != Anchor::Count ? kInfo[IndexOf(step.anchor)].name : step.point ? "the segment's end point" : "a module epoch";
			const bool end = a_step + 1 == info.stepCount;
			const auto& frame = Driver::Current();
			const SegmentFields& fields = frame.Segment(segment);
			if (!fields.declared) {
				Driver::NoteTapError(Driver::TapError::NotDeclared, segment, fmt::format("the {} tap fired", tap));
				return { RunStatus::Idle };
			}
			// A stage's first tap of the frame (present or absent) ends the earlier stages that staged and are still open, at their end point: their commit
			// point did not come (the deferred+water stage's, in a frame Drawcall Limit Fix's forward epoch does not run). Their tickets go
			// first, in recipe order, so this stage's work follows theirs on the GPU.
			// One that ends at a point and staged nothing this frame ends there without a ticket (the start stage in a frame without
			// reflection faces).
			if (a_state.stagedFrame[a_declared] != frame.frame)
				for (std::uint32_t before = 0; before < a_declared; ++before) {
					if (a_state.endedFrame[before] == frame.frame || !a_state.declaredEpochs[before] || !frame.Segment(static_cast<SegmentId>(before)).declared)
						continue;
					if (a_state.stagedFrame[before] != frame.frame && !PointEnded(before))
						continue;
					for (std::uint32_t k = 0; k < kDeclaredInfo[before].stepCount; ++k)
						if (kDeclaredInfo[before].steps[k].point) {
							(void)TapDeclared(a_state, Point{}, before, k, {});
							break;
						}
				}
			if (!fields.graph) {
				// No schedule for this frame slot yet (its graph's first build, or the frames after a build): the stage is absent from
				// the frame, and its work is not drawn until its first schedule is ready. Its end tap still ends it, so the later stages'
				// windows open where they would have.
				Driver::NoteAbsentTap(segment);
				if (end)
					a_state.endedFrame[a_declared] = frame.frame;
				return { RunStatus::Idle };
			}
			if (a_state.endedFrame[a_declared] == frame.frame) {
				Driver::NoteTapError(Driver::TapError::Repeated, segment, fmt::format("the {} tap fired after the segment's end tap", tap));
				return { RunStatus::Idle };
			}
			if (end && !Driver::NoteTap(segment, tap))
				return { RunStatus::Idle };
			// An end point with nothing staged this frame: the stage has no work, and no ticket is submitted.
			if (end && step.point && a_state.stagedFrame[a_declared] != frame.frame) {
				a_state.endedFrame[a_declared] = frame.frame;
				return { RunStatus::Idle };
			}
			auto& host = *a_state.host;
			const std::uint32_t epoch = EpochOfDeclared(a_declared);
			const auto built = a_state.frameBuilt[a_declared];
			// The frame runs on the build its schedule was recorded on. Builds attach only at a frame boundary, before Begin chose it.
			if (!built || fields.graph != host.BuildGeneration(epoch)) {
				Driver::NoteTapError(Driver::TapError::LostToBuild, segment,
					fmt::format("a build of the segment's graph attached after the frame's Begin (schedule build {}, attached {})", fields.graph,
						host.BuildGeneration(epoch)));
				DropStaged(a_state, a_declared);
				if (end)
					a_state.endedFrame[a_declared] = frame.frame;
				return { RunStatus::Failed };
			}
			const SegmentFields& held = fields;
			// The frame's own description, when the schedule lags behind it: work it has on and the schedule off is left out this frame,
			// not a mismatch (the same build, so the client bits name the same clients).
			const SegmentFields& wanted = a_state.frameWanted[a_declared];
			const bool comparable = wanted.graph == held.graph;
			// The clients that stage: this step's clients in the built graph, on in the schedule, with work this frame. An active client
			// the schedule turned off is a mismatch, unless the frame's description has it on (the schedule lags), and its commit is not
			// called.
			std::vector<std::shared_ptr<const ClientDesc>> committed;
			bool foreignOn = false;
			// This tap's instance: the step's first instance and its earlier taps this frame.
			const std::uint32_t local = a_state.stepTaps[a_declared][a_step]++;
			const std::uint32_t instance = step.firstInstance + local;
			if (a_foreign) {
				foreignOn = (held.foreignOn >> a_step & 1u) != 0;
				if (!foreignOn) {
					if (comparable && (wanted.foreignOn >> a_step & 1u))
						Driver::NoteLaggedWork(segment);
					else
						Driver::NoteTapError(Driver::TapError::NotInDescription, segment, fmt::format("{} had work at its tap, and the schedule has it off", tap));
				}
			} else {
				const auto& clients = SegmentClients(*built, a_declared);
				for (std::size_t i = 0; i < clients.size(); ++i) {
					if (!StepHolds(step, *clients[i]))
						continue;
					const auto found = std::ranges::find_if(a_clients, [&](const RunClient& a_client) { return SameClient(clients[i], a_client.desc); });
					const bool active = found != a_clients.end() && found->active;
					const bool on = i < RenderGraph::Frame::kSegmentClients && (held.clientsOn >> i & 1u);
					if (!on) {
						if (active) {
							if (comparable && i < RenderGraph::Frame::kSegmentClients && (wanted.clientsOn >> i & 1u))
								Driver::NoteLaggedWork(segment);
							else
								Driver::NoteTapError(Driver::TapError::NotInDescription, segment,
									fmt::format("{} had work at the {} tap, and the schedule has it off", clients[i]->id, tap));
						}
						continue;
					}
					if (!active)
						continue;
					// The k-th tap of the step writes instance k. A client without instances has one; one with instances has as many
					// as the description declared.
					const std::uint32_t declaredInstances = clients[i]->maxInstances > 1 ? held.instances[i] : clients[i]->repeats ? UINT32_MAX : 1u;
					const bool inRange = clients[i]->maxInstances <= 1 || local < StepInstances(step, *clients[i]).second;
					if (instance >= declaredInstances || !inRange) {
						if (clients[i]->maxInstances > 1 && comparable && instance < wanted.instances[i])
							Driver::NoteLaggedWork(segment);
						else
							Driver::NoteTapError(Driver::TapError::InstanceNotDeclared, segment,
								fmt::format("{} at the {} tap, instance {} of {} declared", clients[i]->id, tap, instance, declaredInstances));
						continue;
					}
					committed.push_back(clients[i]);
				}
			}
			if (step.anchor != Anchor::Count)
				NoteSequenceRun(a_point, !end, a_clients);
			// Stage the tap's values into the frame inputs: the commits write the staging run, which nothing on the GPU reads.
			a_state.stagedFrame[a_declared] = frame.frame;
			UpdateStagesOpen(a_state);
			{
				struct StagingScope
				{
					org::GraphHost& host;
					~StagingScope() { host.EndHeldRun(); }
				} scope{ host };
				RenderGraphRuntime::EpochBodyScope body(info.segment);
				host.UseRun(a_state.stagingRun[a_declared]);
				if (!a_foreign && StepUsesSharedConstants(*built, a_declared, a_step))
					SharedConstants::Commit(a_step);
				if (a_foreign && foreignOn)
					(*a_foreign)();
				a_state.tapInstance = instance;
				for (const auto& client : committed)
					if (client->commit)
						client->commit(a_point);
				a_state.tapInstance = 0;
			}
			if (step.anchor != Anchor::Count) {
				const std::uint32_t index = IndexOf(step.anchor);
				auto& runs = RunsSinceSeal(a_state, index);
				++runs;
				if (runs > a_state.peakRuns[index].load(std::memory_order_relaxed))
					a_state.peakRuns[index].store(runs, std::memory_order_relaxed);
			}
			if (!end) {
				// A staged tap counts as run for its callers, as an early tap of a held ticket did: what it staged reaches the GPU with
				// the stage's commit, before any later GPU work that reads it (the census counts work submitted in between). Last: an
				// executed callback may register, refresh or rearm.
				for (const auto& client : committed)
					if (client->executed)
						client->executed(a_point);
				return { RunStatus::Ran };
			}
			for (const auto& client : committed)
				a_state.stagedExecuted[a_declared].emplace_back(client, a_point);
			a_state.endedFrame[a_declared] = frame.frame;
			return SubmitSegment(a_state, a_point, a_declared);
		}

		/**
		 * @brief The segment that holds a shared anchor's named clients now, and its step there. Its window must be open: the frame
		 * declares it, its end tap has not fired, and every segment before it that the frame declares has ended (or it staged values already
		 * already). kDeclared when none. Render owner.
		 */
		/** @brief Whether a declared segment's window is open now (OpenStepAt). Render owner. */
		bool WindowOpen(const State& a_state, std::uint32_t a_declared)
		{
			const auto& frame = Driver::Current();
			const auto& fields = frame.Segment(static_cast<SegmentId>(a_declared));
			if (!a_state.declaredEpochs[a_declared] || !fields.declared || !fields.graph || a_state.endedFrame[a_declared] == frame.frame)
				return false;
			if (a_state.stagedFrame[a_declared] == frame.frame)
				return true;
			// An earlier stage that ends at a point and has staged nothing yet does not hold this one back: this stage's first tap ends it.
			for (std::uint32_t before = 0; before < a_declared; ++before)
				if (a_state.declaredEpochs[before] && frame.Segment(static_cast<SegmentId>(before)).declared && a_state.endedFrame[before] != frame.frame &&
					!(PointEnded(before) && a_state.stagedFrame[before] != frame.frame))
					return false;
			return true;
		}

		/**
		 * @brief Whether step a_step of a declared segment takes its anchor's taps now: an untilLater step only until a later step of its stage
		 * tapped this frame. Render owner.
		 */
		bool StepTakesTaps(const State& a_state, std::uint32_t a_declared, std::uint32_t a_step)
		{
			const auto& info = kDeclaredInfo[a_declared];
			if (!info.steps[a_step].untilLater || a_state.stagedFrame[a_declared] != Driver::Current().frame)
				return true;
			for (std::uint32_t k = a_step + 1; k < info.stepCount; ++k)
				if (a_state.stepTaps[a_declared][k])
					return false;
			return true;
		}

		/** @brief The step of a declared segment that takes a tap of anchor a_anchor now (StepTakesTaps), the first such; kMaxSteps for none. */
		std::uint32_t TapStepAt(const State& a_state, std::uint32_t a_declared, Anchor a_anchor)
		{
			for (std::uint32_t k = 0; k < kDeclaredInfo[a_declared].stepCount; ++k)
				if (kDeclaredInfo[a_declared].steps[k].anchor == a_anchor && StepTakesTaps(a_state, a_declared, k))
					return k;
			return kMaxSteps;
		}

		std::pair<std::uint32_t, std::uint32_t> OpenStepAt(const State& a_state, Anchor a_anchor)
		{
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if (WindowOpen(a_state, d))
					if (const auto k = TapStepAt(a_state, d, a_anchor); k < kMaxSteps)
						return { d, k };
			return { kDeclared, kMaxSteps };
		}

		/** @brief The step of a declared segment that holds a module epoch, or kMaxSteps. */
		std::uint32_t ForeignStep(std::uint32_t a_declared, std::uint32_t a_segment)
		{
			for (std::uint32_t k = 0; k < kDeclaredInfo[a_declared].stepCount; ++k)
				if (static_cast<std::uint32_t>(kDeclaredInfo[a_declared].steps[k].foreign) == a_segment)
					return k;
			return kMaxSteps;
		}
	}

	namespace
	{
		/** @brief Whether a declared segment is on (SegmentsOn): every CS stage is, in native mode with segments. */
		bool DeclaredOn(std::uint32_t)
		{
			return true;
		}
	}

	void SetForeignPasses(std::uint32_t a_segment, std::function<void(std::uint32_t, void*)> a_gather, std::function<bool()> a_declared)
	{
		if (!HoldsForeign(a_segment))
			return;
		{
			auto& table = Foreign();
			const std::lock_guard lock(table.mutex);
			auto entry = std::make_shared<ForeignPasses>();
			entry->gather = std::move(a_gather);
			entry->declared = std::move(a_declared);
			table.entries[a_segment] = std::move(entry);
		}
		auto lock = Guard();
		auto& state = Anchors();
		if (!state.host)
			return;
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.declaredEpochs[d] && ForeignStep(d, a_segment) != kMaxSteps)
				RenderGraph::Builds::Request(EpochOfDeclared(d), fmt::format("frame segments: epoch {}'s passes joined the {}", a_segment, kDeclaredInfo[d].names.label));
	}

	bool HoldsForeign(std::uint32_t a_segment)
	{
		const auto& state = Anchors();
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.declaredEpochs[d] && ForeignStep(d, a_segment) != kMaxSteps)
				return true;
		return false;
	}

	RunResult TapForeign(std::uint32_t a_segment, const std::function<void()>& a_commit)
	{
		const RunDepthScope depth;
		auto& state = Anchors();
		if (!state.host)
			return { RunStatus::Idle };
		auto lock = Guard();
		if (!IsRenderOwner())
			return { RunStatus::Refused };
		if (!RenderGraphRuntime::Get().IsActive())
			return { RunStatus::Refused };
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			const auto step = state.declaredEpochs[d] ? ForeignStep(d, a_segment) : kMaxSteps;
			if (step == kMaxSteps)
				continue;
			if (!WindowOpen(state, d)) {
				// A segment absent from this frame (no schedule on the attached build yet) draws nothing; that is not a mismatch.
				const auto& fields = Driver::Current().Segment(static_cast<SegmentId>(d));
				if (fields.declared && !fields.graph && state.endedFrame[d] != Driver::Current().frame)
					Driver::NoteAbsentTap(static_cast<SegmentId>(d));
				else
					Driver::NoteTapError(Driver::TapError::NotDeclared, static_cast<SegmentId>(d), fmt::format("the tap of epoch {} fired outside the segment", a_segment));
				return { RunStatus::Idle };
			}
			return TapDeclared(state, Point{}, d, step, {}, &a_commit);
		}
		return { RunStatus::Idle };
	}

	std::uint32_t TapInstance()
	{
		return Anchors().tapInstance;
	}

	bool HeldByStage(Anchor a_anchor)
	{
		const auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_anchor);
		return SegmentsOn() && state.frozen && index < kAnchors && DeclaredOfAnchor(state, index) < kDeclared;
	}

	void DropKeptRecordings(Anchor a_anchor, std::string_view a_why)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_anchor);
		if (!SegmentsOn() || !state.frozen || index >= kAnchors)
			return;
		std::size_t dropped = 0;
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d] || !(WholeMembers(d) >> index & 1u))
				continue;
			// A frame slot's schedule holds its own reference, so the recordings a frame runs on stay until it switches.
			auto& recordings = state.declaredRecordings[d];
			const std::lock_guard recordingsLock(recordings.mutex);
			dropped += std::erase_if(recordings.entries, [](const auto& a_kept) { return a_kept->done; });
		}
		if (dropped)
			logger::info("[ORG] frame segments: dropped {} kept recordings of the stages that hold the {} anchor ({})", dropped, kInfo[index].name, a_why);
	}

	bool NamedByStage(std::string_view a_id)
	{
		const auto& state = Anchors();
		if (!SegmentsOn() || !state.frozen)
			return false;
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d])
				continue;
			for (std::uint32_t k = 0; k < kDeclaredInfo[d].stepCount; ++k)
				for (const char* name : kDeclaredInfo[d].steps[k].clients)
					if (name && a_id == name)
						return true;
		}
		return false;
	}

	bool CanTap(std::string_view a_id)
	{
		auto& state = Anchors();
		if (!SegmentsOn() || !state.host || !IsRenderOwner())
			return false;
		auto lock = Guard();
		auto* client = FindClient(state, a_id);
		if (!client)
			return false;
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d] || !ClientHolds(*client, d) || !WindowOpen(state, d))
				continue;
			const auto& built = state.frameBuilt[d];
			if (built && std::ranges::find(built->clients, client->desc) != built->clients.end())
				return true;
		}
		return false;
	}

	bool StageInputs(std::string_view a_id, const std::function<void()>& a_write)
	{
		auto& state = Anchors();
		if (!SegmentsOn() || !state.host || !IsRenderOwner())
			return false;
		auto lock = Guard();
		auto* client = FindClient(state, a_id);
		if (!client)
			return false;
		const auto& frame = Driver::Current();
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d] || !ClientHolds(*client, d) || !WindowOpen(state, d))
				continue;
			const auto& built = state.frameBuilt[d];
			if (!built || std::ranges::find(built->clients, client->desc) == built->clients.end())
				continue;
			// The client must be on in the frame's schedule: its passes do work only then.
			const auto& clients = SegmentClients(*built, d);
			const auto i = static_cast<std::size_t>(std::ranges::find(clients, client->desc) - clients.begin());
			if (i >= RenderGraph::Frame::kSegmentClients || !(frame.Segment(static_cast<SegmentId>(d)).clientsOn >> i & 1u))
				continue;
			struct StagingScope
			{
				org::GraphHost& host;
				~StagingScope() { host.EndHeldRun(); }
			} scope{ *state.host };
			state.host->UseRun(state.stagingRun[d]);
			state.stagedFrame[d] = frame.frame;
			UpdateStagesOpen(state);
			a_write();
			return true;
		}
		return false;
	}

	std::int32_t StagePlace(std::span<const std::string_view> a_ids)
	{
		auto& state = Anchors();
		if (!SegmentsOn() || !state.host || !IsRenderOwner())
			return -1;
		auto lock = Guard();
		const auto named = [&](const Step& a_step) -> std::int32_t {
			for (const char* id : a_step.clients)
				if (id)
					for (std::size_t i = 0; i < a_ids.size(); ++i)
						if (a_ids[i] == id)
							return static_cast<std::int32_t>(i);
			return -1;
		};
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!WindowOpen(state, d))
				continue;
			const auto& info = kDeclaredInfo[d];
			// The stage's last tapped step this frame (-1: none yet).
			std::int32_t last = -1;
			if (state.stagedFrame[d] == Driver::Current().frame)
				for (std::uint32_t k = 0; k < info.stepCount; ++k)
					if (state.stepTaps[d][k])
						last = static_cast<std::int32_t>(k);
			std::int32_t first = -1, chosen = -1;
			for (std::uint32_t k = 0; k < info.stepCount; ++k) {
				const auto id = named(info.steps[k]);
				if (id < 0)
					continue;
				if (first < 0)
					first = id;
				if (static_cast<std::int32_t>(k) <= last + 1)
					chosen = id;
			}
			if (first >= 0)
				return chosen >= 0 ? chosen : first;
		}
		return -1;
	}

	RunResult TapPoint(std::uint32_t a_segment)
	{
		const RunDepthScope depth;
		auto& state = Anchors();
		if (!state.host)
			return { RunStatus::Idle };
		auto lock = Guard();
		if (!IsRenderOwner() || !RenderGraphRuntime::Get().IsActive())
			return { RunStatus::Refused };
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d] || static_cast<std::uint32_t>(kDeclaredInfo[d].segment) != a_segment)
				continue;
			for (std::uint32_t k = 0; k < kDeclaredInfo[d].stepCount; ++k)
				if (kDeclaredInfo[d].steps[k].point) {
					// An absent segment (no schedule this frame) ends here too, without drawing, so the later windows open.
					const auto& fields = Driver::Current().Segment(static_cast<SegmentId>(d));
					if (fields.declared && !fields.graph && state.endedFrame[d] != Driver::Current().frame)
						return TapDeclared(state, Point{}, d, k, {});
					return WindowOpen(state, d) ? TapDeclared(state, Point{}, d, k, {}) : RunResult{ RunStatus::Idle };
				}
		}
		return { RunStatus::Idle };
	}

	bool DescribingKey()
	{
		return Anchors().describingKey;
	}

	std::optional<bool> DescribedBuildHasRow(std::uint32_t a_row)
	{
		const auto& state = Anchors();
		if (state.describingKey || !state.describingClient || !state.describingAgainst || state.describingAgainst->all || a_row >= 256)
			return std::nullopt;
		const auto [present, rows] = KeyPart(state.describingAgainst, *state.describingClient);
		return present && (rows[a_row / 64] >> (a_row % 64) & 1u) != 0;
	}

	bool SegmentsOn()
	{
		static const bool on = [] {
			if (!RenderGraph::Engine::Native::Requested())
				return false;
			if (!RenderGraphRuntime::EpochsEnabled() || Switches::Equals("CS_ORG_ASYNC_EPOCHS", "0") || Switches::Equals("CS_ORG_REUSE_RECORDINGS", "0")) {
				logger::warn("[ORG] frame segments need epochs, async epochs and replayable recordings; every anchor keeps its own epoch");
				return false;
			}
			logger::info("[ORG] frame segments on: the CS stages' steps run in their epochs, recorded per description");
			return true;
		}();
		return on;
	}

	std::vector<std::uint32_t> SegmentEpochs()
	{
		std::vector<std::uint32_t> epochs;
		const auto& state = Anchors();
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.declaredEpochs[d])
				epochs.push_back(EpochOfDeclared(d));
		return epochs;
	}

	void BeginFrame(RenderGraph::Frame::Description& a_frame)
	{
		auto& state = Anchors();
		if (!SegmentsOn() || !state.host || !IsRenderOwner())
			return;
		auto lock = Guard();
		if (!RenderGraphRuntime::Get().IsActive())
			return;
		using Kind = RenderGraph::Frame::Kind;
		// Values the last frame staged and never committed (its commit point did not come, which the driver counts) are dropped.
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.stagedFrame[d] && state.stagedFrame[d] != a_frame.frame) {
				if (!Driver::LoadStartedThisFrame())
					logger::warn("[ORG] frame segments: the {} staged values that its commit never took by the frame's end; they are dropped",
						kDeclaredInfo[d].names.label);
				DropStaged(state, d);
			}
		// The segments a world frame draws: every one. A frozen world's capture frame draws the world once too. Each takes its frame
		// slot's schedule here, switched to this frame's description when that description's recording is ready, and keeps it to the
		// frame's end.
		// Each frame kind's recipe lists its CS stages (Recipes::KindHas): the world's every stage, the other kinds the UI stage.
		state.inBegin = true;
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			state.frameRecording[d].reset();
			state.frameBuilt[d].reset();
			state.frameWanted[d] = {};
			if (state.declaredEpochs[d]) {
				const bool inRecipe = RenderGraph::Frame::Recipes::KindHas(a_frame.kind, a_frame.capture, static_cast<SegmentId>(d));
				a_frame.segments[d] = Schedule(state, d, inRecipe, a_frame);
				// A present stage's frame inputs start empty for this frame.
				if (state.frameRecording[d])
					OpenInputs(state, d, a_frame.slot);
			}
		}
		// Each stage's key for its next build (absent passes): asked for here, built off the render thread, switched in like any build.
		const bool kindSwitch = state.keyKind != Kind::Count && state.keyKind != a_frame.kind;
		state.keyKind = a_frame.kind;
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.declaredEpochs[d])
				UpdateKey(state, d, RenderGraph::Frame::Recipes::KindHas(a_frame.kind, a_frame.capture, static_cast<SegmentId>(d)), a_frame, kindSwitch);
		state.inBegin = false;
	}

	RunResult Run(const Point& a_point)
	{
		const RunDepthScope depth;
		auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_point.anchor);
		if (index >= kAnchors || !state.reserved[index] || !state.host)
			return { RunStatus::Idle };
		// Ownership is checked under the mutex a claim takes: it cannot move in the middle of this run.
		auto lock = Guard();
		if (!IsRenderOwner())
			return { RunStatus::Refused };
		auto& runtime = RenderGraphRuntime::Get();
		if (!runtime.IsActive())
			return { RunStatus::Refused };
		ServiceRetiredTextures();
		// Before the clients are read: an anchor whose epoch failed drops its clients here (OnEpochFailed), not in the middle of a run.
		runtime.PollEpochFailures();
		if (!runtime.IsActive())
			return { RunStatus::Failed };
		Service(state);
		// The clients in this anchor's graph; the run happens when one is active or has a one-shot pass pending. A client waiting to
		// join (RequestJoin) does not run the epoch: its graph is built at the next build point.
		std::vector<RunClient> clients;
		bool work = false;
		for (const auto& client : state.clients) {
			if (!InGraphs(*client.desc) || !HasPasses(*client.desc, a_point.anchor))
				continue;
			const bool active = !client.desc->active || client.desc->active();
			work = work || active || HasPendingOnce(client, a_point.anchor);
			clients.push_back({ client.desc, client.serial, active });
		}
		// Declared segments: an anchor a segment holds wholly is one of its taps whenever it fires.
		if (const auto declared = DeclaredOfAnchor(state, index); declared < kDeclared) {
			const auto step = TapStepAt(state, declared, a_point.anchor);
			if (step >= kMaxSteps) {
				Driver::NoteTapError(Driver::TapError::Repeated, static_cast<SegmentId>(declared), fmt::format("the {} tap fired after its steps took their last tap", kInfo[index].name));
				return { RunStatus::Idle };
			}
			return TapDeclared(state, a_point, declared, step, clients);
		}
		// A shared anchor: the clients a segment names commit into it while its window is open. The others run the anchor's own epoch.
		// A segment-only client (ClientDesc::segmentOnly) has no passes there: with work outside every open window it draws nothing.
		const auto dropSegmentOnly = [&](std::vector<RunClient>& a_clients, std::string_view a_why) {
			for (auto& client : a_clients)
				if (client.active && client.desc->segmentOnly) {
					Driver::NoteTapError(Driver::TapError::NotDeclared, SegmentId::Count, fmt::format("{} had work at the {} tap {}", client.desc->id, kInfo[index].name, a_why));
					client.active = false;
				}
		};
		if (SegmentsOn() && work) {
			if (OpenStepAt(state, a_point.anchor).first < kDeclared) {
				std::vector<RunClient> others = clients;
				std::vector<RunClient> members;
				RunResult tapped{ RunStatus::Idle };
				// Every open segment with a step at this anchor takes its own clients. Two can be open at once: a stage whose commit
				// point has not come yet (the deferred+water stage in a frame Drawcall Limit Fix's forward epoch does not run) and the
				// post stage after it, which has staged this frame.
				for (std::uint32_t declared = 0; declared < kDeclared; ++declared) {
					const auto first = StepAt(declared, a_point.anchor);
					if (first == kMaxSteps || !WindowOpen(state, declared))
						continue;
					const auto& built = state.frameBuilt[declared];
					// Every step of the segment at this anchor takes its own clients (the post stage has three ImageSpace steps), each
					// tapped when one of them has work.
					for (std::uint32_t k = first; k < kDeclaredInfo[declared].stepCount; ++k) {
						const auto& stepInfo = kDeclaredInfo[declared].steps[k];
						if (stepInfo.anchor != a_point.anchor || !StepTakesTaps(state, declared, k))
							continue;
						std::vector<RunClient> stepMembers;
						for (auto it = others.begin(); it != others.end();) {
							const bool member = built && StepHolds(stepInfo, *it->desc) &&
							                    std::ranges::any_of(built->clients, [&](const auto& a_built) { return SameClient(a_built, it->desc); });
							if (member) {
								stepMembers.push_back(*it);
								it = others.erase(it);
							} else {
								++it;
							}
						}
						// A client that commits once per segment run, active at a second run of its step: a mismatch. That run draws nothing.
						const bool again = state.stagedFrame[declared] == Driver::Current().frame && state.stepTaps[declared][k] > 0 &&
						                   std::ranges::any_of(stepMembers, [](const RunClient& a_client) {
											   return a_client.active && !a_client.desc->repeats && a_client.desc->maxInstances <= 1;
										   });
						if (again) {
							Driver::NoteTapError(Driver::TapError::Repeated, static_cast<RenderGraph::Frame::SegmentId>(declared), fmt::format("the {} step ran again in one segment run", kInfo[index].name));
							break;
						}
						if (std::ranges::any_of(stepMembers, [](const RunClient& a_client) { return a_client.active; })) {
							const auto result = TapDeclared(state, a_point, declared, k, stepMembers);
							if (tapped.status == RunStatus::Idle || !result)
								tapped = result;
						}
						members.insert(members.end(), stepMembers.begin(), stepMembers.end());
					}
				}
				dropSegmentOnly(others, "outside its segment's built graph");
				const bool otherWork = std::ranges::any_of(others, [&](const RunClient& a_client) {
					return a_client.active || HasPendingOnce(*FindClient(state, a_client.desc->id), a_point.anchor);
				});
				if (!otherWork)
					return tapped;
				// The rest run now in the anchor's own epoch: ahead of the segment's work on the GPU (counted, BeforeGpuWork).
				clients = std::move(others);
				for (auto& member : members) {
					member.active = false;
					clients.push_back(member);
				}
			} else {
				dropSegmentOnly(clients, "outside its segment's window");
				work = std::ranges::any_of(clients, [&](const RunClient& a_client) {
					return a_client.active || HasPendingOnce(*FindClient(state, a_client.desc->id), a_point.anchor);
				});
			}
		}
		if (!work)
			return { RunStatus::Idle };
		// Every run takes a run lease of the host's: an anchor may run any number of times a frame without waiting for one of its
		// runs of the same frame, which DXVK has not submitted yet. The runs are counted for the report alone (LogRunStats).
		auto& runs = RunsSinceSeal(state, index);
		const auto segment = static_cast<Segment>(SegmentOf(a_point.anchor));
		// The graph holds the upload whenever a registered client reads the shared constants, active or not.
		std::vector<std::shared_ptr<const ClientDesc>> descs;
		for (const auto& client : clients)
			descs.push_back(client.desc);
		const bool sharedConstants = UsesSharedConstants(descs, a_point.anchor);
		std::uint64_t built = 0;
		RenderGraphRuntime::EpochResult result = RenderGraphRuntime::EpochResult::Stopped;
		// The stage commit points and the open-stage census see this epoch here, named for the anchor (TryExecuteEpoch's own call
		// for it counts nothing then).
		BeforeGpuWork(Interned(fmt::format("an anchor with its own epoch ({})", kInfo[index].name)), static_cast<std::uint32_t>(segment));
		// TryExecuteEpoch calls BeforeGpuWork for the same submission: the census counts it once, here.
		state.insideHeldCounted = static_cast<std::uint32_t>(segment);
		NoteSequenceRun(a_point, false, clients);
		{
			RenderGraphRuntime::EpochBodyScope body(segment);
			result = runtime.TryExecuteEpoch(segment, [&](org::RenderGraph&) {
				// The graph this run submits is built by now.
				built = state.built[index].load(std::memory_order_acquire);
				RenderGraph::RunShapes::BeginRun();
				if (sharedConstants)
					SharedConstants::Commit();
				// Every gate closed first: a run does work only where this run's commits open it. That includes the gates of
				// registrations whose passes this graph still holds after they left (RetiredGates).
				for (const auto& client : clients)
					for (const auto& gate : client.desc->gates)
						if (gate)
							gate->Close();
				for (const auto& retired : state.retired)
					if ((retired.mask >> index & 1u) && built < retired.serial)
						for (const auto& gate : retired.gates)
							gate->Close();
				for (const auto& client : clients)
					if (client.active && client.desc->commit)
						client.desc->commit(a_point);
			});
		}
		state.insideHeldCounted = UINT32_MAX;
		if (result == RenderGraphRuntime::EpochResult::Skipped) {
			// Only a revision-driven epoch is skipped (its published copies were all busy), never an anchor's live one: nothing ran,
			// nothing is dropped.
			return { RunStatus::Refused };
		}
		if (result == RenderGraphRuntime::EpochResult::EpochFailed) {
			// Only this anchor's epoch failed (ExecuteEpoch logged why). Its clients are dropped at the next poll (OnEpochFailed); the
			// other anchors and their clients run on.
			return { RunStatus::Failed };
		}
		if (result != RenderGraphRuntime::EpochResult::Submitted) {
			// The graph failed and stopped (ExecuteEpoch logged why): every client returns to its D3D11 path.
			StopAll(state, Name(a_point.anchor));
			return { RunStatus::Failed };
		}
		// The run is counted, and the peak reported (LogRunStats).
		++runs;
		if (runs > state.peakRuns[index].load(std::memory_order_relaxed))
			state.peakRuns[index].store(runs, std::memory_order_relaxed);
		if (RenderGraph::RunShapes::On()) {
			// The run's shape (CS_ORG_EPOCH_STATS): its gate entries (noted as the commits opened them), targets and active clients.
			std::uint64_t active = 0;
			for (const auto& client : clients)
				if (client.active)
					active += reinterpret_cast<std::uintptr_t>(client.desc.get()) * 0x9E3779B97F4A7C15ull;
			RenderGraph::RunShapes::EndRun(index, kInfo[index].name, a_point.input, a_point.output, active);
		}
		// An epoch must not be the last GPU work before a third-party Vulkan consumer (render-graph.md, "Hazard: external Vulkan
		// work directly after a graph epoch"). After the late anchors an upscaler, frame generation or the present may follow.
		if (kInfo[index].late && state.lateHook)
			state.lateHook();
		if (sharedConstants)
			SharedConstants::CheckAfterRun();
		PruneRetired(state);
		// One-shot passes in the graph this run submitted have run: the next build leaves them out.
		bool ranOnce = false;
		for (auto& client : state.clients)
			for (std::size_t i = 0; i < client.desc->passes.size(); ++i)
				if (client.armed[i] && client.armed[i] <= built && client.desc->passes[i].anchor == a_point.anchor) {
					client.armed[i] = 0;
					ranOnce = true;
				}
		if (ranOnce) {
			Publish(state);
			Rebuild(state, 1u << index, "one-shot passes ran");
		}
		// Last: an executed callback may register, refresh or rearm. A matchesById client whose newer registration is not built yet ran
		// through the registration the graph holds (its commit writes what every registration's passes read).
		const auto ownBuilt = OwnSnapshot(state, index);
		const auto heldById = [&](const RunClient& a_client) {
			return a_client.desc->matchesById && ownBuilt &&
			       std::ranges::any_of(ownBuilt->clients, [&](const auto& a_built) { return a_built->id == a_client.desc->id; });
		};
		for (const auto& client : clients)
			if (client.active && client.desc->executed && (client.serial <= built || heldById(client)))
				client.desc->executed(a_point);
		return { RunStatus::Ran };
	}

	bool RequestJoin(std::string_view a_id, Anchor a_anchor)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_anchor);
		const auto* client = FindClient(state, a_id);
		if (!client || index >= kAnchors || !state.reserved[index] || !HasPasses(*client->desc, a_anchor))
			return false;
		if (client->serial <= BuiltSerial(state, index))
			return true;
		// Register or Refresh asked for this build already. Asked again only when no request is pending: a request each frame while
		// the build runs off the render thread would supersede it at every attach. The build manager builds it; its attach records it.
		// With regions, the anchor's own graph and its region's are both asked for: either may be the one to hold its passes.
		const auto epochs = EpochsOf(state, 1u << index);
		if (state.host && std::ranges::none_of(epochs, [&](std::uint32_t a_epoch) { return state.host->RebuildRequested(a_epoch); }))
			Rebuild(state, 1u << index, fmt::format("client '{}' asked to join", a_id));
		return false;
	}

	bool CanRun(std::string_view a_id, Anchor a_anchor)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_anchor);
		if (index >= kAnchors || !state.reserved[index] || !state.host || !IsRenderOwner() || !RenderGraphRuntime::Get().IsActive())
			return false;
		const auto* client = FindClient(state, a_id);
		if (!client || !InGraphs(*client->desc) || client->serial > BuiltSerial(state, index))
			return false;
		// An anchor a declared segment holds wholly draws only on the frame's schedule: the segment is present (not absent while its
		// first schedule on this build is recorded), the schedule's graph holds this registration, and has the client on.
		if (const auto declared = DeclaredOfAnchor(state, index); declared < kDeclared) {
			const auto& fields = Driver::Current().Segment(static_cast<SegmentId>(declared));
			const auto& built = state.frameBuilt[declared];
			if (!fields.declared || !fields.graph || !built)
				return false;
			const auto& clients = SegmentClients(*built, declared);
			const auto found = std::ranges::find(clients, client->desc);
			const auto i = static_cast<std::size_t>(found - clients.begin());
			return found != clients.end() && i < RenderGraph::Frame::kSegmentClients && (fields.clientsOn >> i & 1u);
		}
		return true;
	}

	bool InStage(std::string_view a_id, Anchor a_anchor)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_anchor);
		if (!SegmentsOn() || !state.host || index >= kAnchors)
			return false;
		const auto declared = DeclaredOfAnchor(state, index);
		if (declared >= kDeclared)
			return false;
		const auto* client = FindClient(state, a_id);
		if (!client)
			return false;
		const auto holds = [&](const std::shared_ptr<const Snapshot>& a_built) {
			return a_built && std::ranges::any_of(a_built->clients, [&](const auto& a_held) { return SameClient(a_held, client->desc); });
		};
		const auto& last = state.frameBuilt[declared];
		return last ? holds(last) : holds(state.declaredSnapshots[declared].load(std::memory_order_acquire));
	}

	StageHold HeldNow(std::string_view a_id, Anchor a_anchor)
	{
		auto lock = Guard();
		auto& state = Anchors();
		StageHold hold;
		if (!SegmentsOn() || !state.host || !IsRenderOwner())
			return hold;
		// The open stage's step at the anchor that takes taps now and holds the client (a shared anchor has a step per client group).
		const auto* registered = FindClient(state, a_id);
		std::uint32_t declared = kDeclared, step = kMaxSteps;
		for (std::uint32_t d = 0; d < kDeclared && declared == kDeclared; ++d) {
			if (!WindowOpen(state, d))
				continue;
			for (std::uint32_t k = 0; k < kDeclaredInfo[d].stepCount; ++k)
				if (kDeclaredInfo[d].steps[k].anchor == a_anchor && StepTakesTaps(state, d, k) && registered && StepHolds(kDeclaredInfo[d].steps[k], *registered->desc)) {
					declared = d;
					step = k;
					break;
				}
		}
		if (declared >= kDeclared)
			return hold;
		hold.held = true;
		const auto& fields = Driver::Current().Segment(static_cast<SegmentId>(declared));
		const auto& built = state.frameBuilt[declared];
		if (!fields.declared || !fields.graph || !built)
			return hold;
		const auto& clients = SegmentClients(*built, declared);
		for (std::size_t i = 0; i < clients.size() && i < RenderGraph::Frame::kSegmentClients; ++i)
			if (clients[i]->id == a_id && StepHolds(kDeclaredInfo[declared].steps[step], *clients[i])) {
				hold.scheduled = (fields.clientsOn >> i & 1u) != 0;
				hold.generation = clients[i]->generation;
			}
		return hold;
	}

	std::uint32_t PeakRunsPerFrame(Anchor a_anchor)
	{
		return IndexOf(a_anchor) < kAnchors ? Anchors().peakRuns[IndexOf(a_anchor)].load(std::memory_order_relaxed) : 0;
	}

	bool BuildPending(std::string_view a_caller)
	{
		auto& state = Anchors();
		if (!state.host || !IsRenderOwner()) {
			logger::error("[ORG] frame anchors: {} asked for the graphs off the render owner; its clients join at the first build point", a_caller);
			return false;
		}
		auto lock = Guard();
		auto& runtime = RenderGraphRuntime::Get();
		if (!runtime.IsActive())
			return false;
		const auto waiting = [&] { return std::ranges::count_if(state.clients, [&](const Client& a_client) { return WaitsToJoin(state, a_client); }); };
		if (waiting() == 0)
			return true;
		// Before the first world frame the graphs are built here; after it they are built off the render thread, and the clients join
		// when the builds are attached (each attach records its anchor's snapshot).
		const bool builtNow = runtime.BuildSetupGraphs(a_caller);
		const auto left = waiting();
		if (left == 0)
			logger::info("[ORG] frame anchors: graphs built at {}; every registered client is in its anchors' graphs", a_caller);
		else if (!builtNow && runtime.IsActive())
			logger::info("[ORG] frame anchors: {}: {} clients join their anchors' graphs when the builds asked for are attached", a_caller, left);
		else
			logger::error("[ORG] frame anchors: graphs built at {}, but {} clients are still not in their anchors' graphs", a_caller, left);
		return left == 0;
	}

	namespace
	{
		/**
		 * @brief PrimeStages for one UI stage (a_segment): its description now, recorded for every frame slot on the host's thread while setup
		 * waits, installed as each slot's schedule.
		 */
		void PrimeStage(State& state, Segment a_segment, std::string_view a_caller)
		{
			using namespace std::chrono_literals;
			std::uint32_t ui = kDeclared;
			RenderGraph::Frame::SegmentFields wanted{};
			std::shared_ptr<const Snapshot> built;
			std::array<std::shared_ptr<SegmentRecording>, RenderGraphRuntime::kGameFramesInFlight> entries{};
			{
				auto lock = Guard();
				if (!RenderGraphRuntime::Get().IsActive())
					return;
				for (std::uint32_t d = 0; d < kDeclared; ++d)
					if (state.declaredEpochs[d] && kDeclaredInfo[d].segment == a_segment)
						ui = d;
				if (ui == kDeclared || state.host->StagedGeneration(EpochOfDeclared(ui)) != 0)
					return;
				state.inBegin = true;
				wanted = DescribeDeclared(state, ui, true);
				built = state.declaredSnapshots[ui].load(std::memory_order_acquire);
				if (wanted.graph)
					for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight; ++s)
						entries[s] = RequestDeclared(state, ui, wanted, s, 0, fmt::format(" (the first frame's, at {})", a_caller));
				state.inBegin = false;
			}
			if (!wanted.graph)
				return;
			// At setup, before the first frame: the host's thread records them; the first Begin finds them done and switches them in, so the
			// first frame draws the stage (the session's first movie frame among it). The render thread never waits for a recording in a frame.
			auto& recordings = state.declaredRecordings[ui];
			const auto deadline = std::chrono::steady_clock::now() + 10s;
			bool done = false;
			while (!done && std::chrono::steady_clock::now() < deadline) {
				{
					const std::lock_guard lock(recordings.mutex);
					done = std::ranges::all_of(entries, [](const auto& a_entry) { return a_entry && a_entry->done; });
				}
				if (!done)
					std::this_thread::sleep_for(1ms);
			}
			auto lock = Guard();
			std::uint32_t ready = 0;
			for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight; ++s) {
				std::shared_ptr<const org::GraphHost::EpochRecording> recording;
				{
					const std::lock_guard recordingsLock(recordings.mutex);
					if (entries[s]->done)
						recording = entries[s]->recording;
				}
				if (recording && state.host->EpochRecordingCurrent(*recording)) {
					state.schedule[ui][s] = State::Scheduled{ wanted, std::move(recording), built };
					++ready;
				}
			}
			if (ready == RenderGraphRuntime::kGameFramesInFlight)
				logger::info("[ORG] frame segments: the {}'s first schedule is recorded at {} for every frame slot (build {}); the first frame of its kinds draws it",
					kDeclaredInfo[ui].names.label, a_caller, wanted.graph);
			else
				logger::error("[ORG] frame segments: the {}'s first schedule was recorded for {} of {} frame slots at {}; the others switch in at their first Begin",
					kDeclaredInfo[ui].names.label, ready, RenderGraphRuntime::kGameFramesInFlight, a_caller);
		}
	}

	void PrimeStages(std::string_view a_caller)
	{
		auto& state = Anchors();
		if (!SegmentsOn() || !state.host || !IsRenderOwner())
			return;
		// The session's first frames are a Bink movie's (the UI stage), then the main menu's (the menu UI stage): both are ready before them.
		PrimeStage(state, Segment::DeclaredUi, a_caller);
		PrimeStage(state, Segment::DeclaredMenuUi, a_caller);
	}

	void LogRunStats()
	{
		auto& state = Anchors();
		{
			auto lock = Guard();
			std::string inside;
			for (auto& [why, count] : state.insideHeld)
				if (count)
					inside += fmt::format("{}{} {}", inside.empty() ? "" : ", ", why, std::exchange(count, 0));
			logger::info("[ORG] frame segments: GPU work submitted between a stage's first tap and its commit, since the last report: {}", inside.empty() ? "none" : inside);
		}
		std::string runs;
		for (std::uint32_t i = 0; i < kAnchors; ++i)
			if (state.reserved[i] && state.frozen)
				runs += fmt::format("{}{} {}", runs.empty() ? "" : ", ", kInfo[i].name, state.peakRuns[i].load(std::memory_order_relaxed));
		if (!runs.empty())
			logger::info("[ORG] frame anchors, peak runs in one frame: {}", runs);
	}

	Texture EngineTarget(std::uint32_t a_target)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || a_target >= static_cast<std::uint32_t>(RE::RENDER_TARGET::kTOTAL))
			return {};
		const auto target = renderer->GetRuntimeData().renderTargets[a_target];
		// kFRAMEBUFFER is the swap chain's back buffer: the engine keeps only its RTV, so the texture comes from the view.
		auto* texture = target.texture;
		winrt::com_ptr<ID3D11Texture2D> fromView;
		if (!texture && target.RTV) {
			winrt::com_ptr<ID3D11Resource> resource;
			reinterpret_cast<ID3D11RenderTargetView*>(target.RTV)->GetResource(resource.put());
			if (resource)
				fromView = resource.try_as<ID3D11Texture2D>();
			texture = reinterpret_cast<decltype(texture)>(fromView.get());
		}
		return Import(texture, fmt::format("cs.engine.target.{}", a_target),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeColour(a_desc, target.SRV, target.RTV, target.UAV); });
	}

	Texture EngineDepth(std::uint32_t a_depthStencil)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || a_depthStencil >= static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL))
			return {};
		const auto target = renderer->GetDepthStencilData().depthStencils[a_depthStencil];
		return Import(target.texture, fmt::format("cs.engine.depth.{}", a_depthStencil),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeDepth(target, a_desc); });
	}

	Texture EngineCubemap(std::uint32_t a_cubemap)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || a_cubemap >= static_cast<std::uint32_t>(RE::RENDER_TARGETS_CUBEMAP::kTOTAL))
			return {};
		const auto target = renderer->GetRendererData().cubemapRenderTargets[a_cubemap];
		// The faces' RTVs share one format; the graph makes its own views of each face.
		return Import(target.texture, fmt::format("cs.engine.cubemap.{}", a_cubemap),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeColour(a_desc, target.SRV, target.cubeSideRTV[0], nullptr); });
	}

	Texture D3D11Texture(std::string_view a_name, ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_srv,
		ID3D11UnorderedAccessView* a_uav, ID3D11RenderTargetView* a_rtv)
	{
		if (a_name.empty())
			return {};
		return Import(a_texture, fmt::format("cs.d3d11.{}", a_name),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeColour(a_desc, a_srv, a_rtv, a_uav); });
	}

	Texture ImmutableTexture(std::string_view a_name, ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_srv)
	{
		// Native mode: an exempt texture is one graph resource, read untracked, the same pointer on every call (no build). It is a
		// plain resource, declared as itself.
		if (RenderGraph::Engine::Native::Active() && a_texture && RenderGraphRuntime::Get().IsActive()) {
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			if (a_srv)
				a_srv->GetDesc(&view);
			if (auto exempt = RenderGraph::Engine::Native::ImmutableTextureOf(a_texture, a_srv ? view.Format : DXGI_FORMAT_UNKNOWN))
				return Texture(std::move(exempt));
		}
		return D3D11Texture(a_name, a_texture, a_srv, nullptr);
	}

	Texture ImportDepth(const RE::BSGraphics::DepthStencilData& a_target, const std::string& a_id)
	{
		return Import(a_target.texture, a_id, [&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeDepth(a_target, a_desc); });
	}

	Texture ImportTexture(std::string_view a_id, ID3D11Texture2D* a_texture, const org::TextureDescription& a_views)
	{
		if (a_id.empty())
			return {};
		return Import(a_texture, std::string(a_id), [&](const D3D11_TEXTURE2D_DESC&) { return a_views; });
	}

	Texture ShareImage(ID3D11Texture2D* a_texture, const DxvkOrgInteropImageInfo& a_image, const org::TextureDescription& a_desc, std::string_view a_name)
	{
		auto lock = Guard();
		auto& state = Anchors();
		auto& runtime = RenderGraphRuntime::Get();
		const auto image = reinterpret_cast<std::uint64_t>(a_image.image);
		if (!image || !runtime.IsActive() || !runtime.Host())
			return {};
		// An engine target ORG owns (CS_ORG_ENGINE_TARGETS): its published slot, the one the anchors' passes use.
		if (auto owned = RenderGraph::Engine::Targets::FindImage(image))
			return owned;
		auto desc = a_desc;
		DescribeSubresources(desc, a_image);
		const std::string name(a_name);
		// An image ORG made with a graph resource of its own: that resource (a versioned twin's slot).
		if (auto made = MadeTexture(a_texture, image, desc, name))
			return made;
		// The slot an importer already made for the image, with the caller's views added when it lacks one.
		if (auto shared = FindImage(state, image)) {
			EnsureViews(shared, desc, name);
			if (auto slot = shared->Slot())
				return Texture(shared->id, std::move(slot));
		}
		auto entry = std::make_shared<EngineImport>();
		entry->texture.copy_from(a_texture);
		entry->id = name;
		entry->image = image;
		entry->info = a_image;
		auto backing = MakeBacking(a_texture, a_image, desc, name);
		if (!backing)
			return {};
		entry->views = desc;
		// The version keeps the entry, and with it the D3D11 texture: the image lives, and its handle is not reused, while the slot does.
		auto slot = org::VersionedTexture::CreateImage(std::move(backing), entry);
		entry->looseSlot = slot;
		state.loose.push_back(entry);
		return Texture(name, std::move(slot));
	}

	void ReleaseImport(std::string_view a_id)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const auto found = state.imports.find(a_id);
		if (found == state.imports.end())
			return;
		// The graph resource views DXVK's image, whose memory the D3D11 texture owns: both go once the GPU is past every use,
		// which the graph's in-stream epochs and their compute joins precede. Another id of the same image keeps both.
		auto owner = std::move(found->second);
		state.imports.erase(found);
		Publish(state);
		StructuralRebuild(state, ~0u, fmt::format("import '{}' released", a_id));
		ReleaseUnnamed(state, std::move(owner));
	}

	void OnEpochFailed(std::uint32_t a_epoch, const std::string& a_error, bool a_keptGraph)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const Anchor anchor = AnchorOfEpoch(a_epoch);
		if (const auto declared = anchor == Anchor::Count ? DeclaredOfEpoch(a_epoch) : kDeclared; declared != kDeclared) {
			// A declared segment's epoch: the clients of its members in the failed build are dropped, as for an anchor's.
			const auto failed = state.declaredSnapshots[declared].load(std::memory_order_acquire);
			if (a_keptGraph)
				state.declaredSnapshots[declared].store(state.declaredPrevious[declared].load(std::memory_order_acquire), std::memory_order_release);
			DropStaged(state, declared);
			DropFailedClients(StepAnchors(declared), kDeclaredInfo[declared].names.label, failed, a_error);
			return;
		}
		if (anchor == Anchor::Count)
			return;
		const std::uint32_t index = IndexOf(anchor);
		// The host's own build or preparation failed (a build on the owner thread records its snapshot at once).
		const auto failed = state.builtSnapshots[index].load(std::memory_order_acquire);
		if (a_keptGraph) {
			// The previous graph still runs: what it was built from is the anchor's again.
			const auto previous = state.previousSnapshots[index].load(std::memory_order_acquire);
			state.builtSnapshots[index].store(previous, std::memory_order_release);
			state.built[index].store(previous ? previous->serial : 0, std::memory_order_release);
		}
		DropFailedClients(1u << index, kInfo[index].name, failed, a_error);
	}

	void AddToFrame(std::vector<std::uint32_t>& a_epochs)
	{
		auto& state = Anchors();
		if (!RenderGraphRuntime::EpochsEnabled())
			return;
		state.frozen = true;
		// Declared segments: an epoch for each that is on and has a reserved anchor; the anchors it holds wholly have none of their
		// own. Its end tap's anchor is reserved with it, so the hook that ends it always runs.
		if (SegmentsOn())
			for (std::uint32_t d = 0; d < kDeclared; ++d) {
				if (!DeclaredOn(d))
					continue;
				std::uint32_t members = 0;
				bool any = false;
				for (std::uint32_t i = 0; i < kAnchors; ++i) {
					any = any || (state.reserved[i] && (StepAnchors(d) >> i & 1u));
					if (state.reserved[i] && (WholeMembers(d) >> i & 1u))
						members |= 1u << i;
				}
				if (!any)
					continue;
				if (const auto end = EndTap(d); end != Anchor::Count) {
					state.reserved[IndexOf(end)] = true;
					members |= 1u << IndexOf(end);
				}
				state.declaredEpochs[d] = true;
				state.declaredMembers |= members;
				const auto segment = static_cast<std::uint32_t>(kDeclaredInfo[d].segment);
				const std::uint32_t epoch = RenderGraphRuntime::EpochOf(kDeclaredInfo[d].segment);
				if (std::ranges::find(a_epochs, epoch) != a_epochs.end())
					continue;
				const auto after = std::ranges::find_if(a_epochs, [&](std::uint32_t a_epoch) { return RankOf(a_epoch) > RankOf(segment); });
				a_epochs.insert(after, epoch);
			}
		for (std::uint32_t i = 0; i < kAnchors; ++i) {
			if (!state.reserved[i] || (state.declaredMembers >> i & 1u))
				continue;
			const std::uint32_t segment = SegmentOf(static_cast<Anchor>(i));
			const std::uint32_t epoch = RenderGraphRuntime::EpochOf(static_cast<Segment>(segment));
			if (std::ranges::find(a_epochs, epoch) != a_epochs.end())
				continue;
			// Before the first epoch the frame runs after it (the epochs are segment values here).
			const auto after = std::ranges::find_if(a_epochs, [&](std::uint32_t a_epoch) { return RankOf(a_epoch) > RankOf(segment); });
			a_epochs.insert(after, epoch);
		}
	}

	void SetTicketsAhead(org::GraphHost& a_host)
	{
		auto& state = Anchors();
		if (!RenderGraphRuntime::EpochsEnabled())
			return;
		// An anchor that runs ten times a frame back to back would otherwise wait for a preparation at each run.
		const auto most = static_cast<std::uint32_t>((std::min)(Switches::Number("CS_ORG_TICKETS_AHEAD", 16), std::uint64_t{ 64 }));
		for (std::uint32_t i = 0; i < kAnchors; ++i)
			if (state.reserved[i] && !(state.declaredMembers >> i & 1u))
				a_host.SetEpochTicketsAhead(EpochOfAnchor(static_cast<Anchor>(i)), most);
	}

	void Attach(org::GraphHost& a_host)
	{
		auto& state = Anchors();
		if (std::ranges::none_of(state.reserved, [](bool a_reserved) { return a_reserved; }))
			return;
		state.host = &a_host;
		Publish(state);
		// Made at every graph build from the snapshot published then; with no graph built yet, adding it rebuilds nothing.
		a_host.AddExtension(kExtensionId, [&state]() -> std::unique_ptr<org::RenderGraph::IRenderGraphExtension> {
			return std::make_unique<AnchorExtension>(state.snapshot.load(std::memory_order_acquire));
		});
		std::string reserved;
		for (std::uint32_t i = 0; i < kAnchors; ++i)
			if (state.reserved[i]) {
				const auto d = DeclaredOfAnchor(state, i);
				reserved += fmt::format("{}{} (epoch {}{})", reserved.empty() ? "" : ", ", kInfo[i].name, d < kDeclared ? EpochOfDeclared(d) : EpochOfAnchor(static_cast<Anchor>(i)),
					d < kDeclared ? fmt::format(", in the {}", kDeclaredInfo[d].names.label) : std::string());
			}
		logger::info("[ORG] frame anchors with epochs: {}", reserved);
	}
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: no anchor has an epoch, and every hook does nothing.
#	include "RenderGraph/FrameAnchors.h"

namespace FrameAnchors
{
	const char* Name(Anchor) { return "unknown"; }
	bool Reserve(Anchor) { return false; }
	bool HasEpoch(Anchor) { return false; }
	bool RecordsAtExecution(Anchor) { return false; }
	bool GpuGates() { return false; }
	bool Register(ClientDesc) { return false; }
	void Unregister(std::string_view) {}
	void Refresh(std::string_view) {}
	RunResult Run(const Point&) { return {}; }
	bool InRun() { return false; }
	bool SegmentsOn() { return false; }
	bool DescribingKey() { return false; }
	std::optional<bool> DescribedBuildHasRow(std::uint32_t) { return std::nullopt; }
	bool IsMainThread() { return true; }
	std::uint32_t RunSegment(Anchor) { return 0; }
	void BeginFrame(RenderGraph::Frame::Description&) {}
	std::vector<std::uint32_t> SegmentEpochs() { return {}; }
	void BeforeGpuWork(const char*, std::uint32_t) {}
	void LatchMainThread() {}
	bool IsRenderOwner() { return true; }
	void ClaimRenderOwnership() {}
	bool RequestJoin(std::string_view, Anchor) { return false; }
	bool CanRun(std::string_view, Anchor) { return false; }
	bool HeldByStage(Anchor) { return false; }
	void DropKeptRecordings(Anchor, std::string_view) {}
	bool NamedByStage(std::string_view) { return false; }
	std::uint32_t PeakRunsPerFrame(Anchor) { return 0; }
	bool BuildPending(std::string_view) { return false; }
	void PrimeStages(std::string_view) {}
	void LogRunStats() {}
	std::uint64_t RenderOwnerChanges() { return 0; }
	Texture::Texture(std::string a_id, std::shared_ptr<org::VersionedTexture> a_slot) :
		id(std::move(a_id)), slot(std::move(a_slot)) {}
	std::shared_ptr<org::GloballyIndexedResource> Texture::Current() const { return nullptr; }
	org::GloballyIndexedResource* Texture::operator->() const { return nullptr; }
	std::uint32_t Texture::Width() const { return 0; }
	std::uint32_t Texture::Height() const { return 0; }
	rhi::Format Texture::Format() const { return rhi::Format{}; }
	bool Texture::SameBacking(const Texture& a_other) const { return *this == a_other; }
	const org::IResourceResolver* Texture::DeclaredResolver() const { return nullptr; }
	Texture EngineTarget(std::uint32_t) { return {}; }
	Texture EngineDepth(std::uint32_t) { return {}; }
	Texture EngineCubemap(std::uint32_t) { return {}; }
	Texture D3D11Texture(std::string_view, ID3D11Texture2D*, ID3D11ShaderResourceView*, ID3D11UnorderedAccessView*, ID3D11RenderTargetView*) { return {}; }
	Texture ImmutableTexture(std::string_view, ID3D11Texture2D*, ID3D11ShaderResourceView*) { return {}; }
	Texture ImportDepth(const RE::BSGraphics::DepthStencilData&, const std::string&) { return {}; }
	Texture ImportTexture(std::string_view, ID3D11Texture2D*, const org::TextureDescription&) { return {}; }
	Texture ShareImage(ID3D11Texture2D*, const DxvkOrgInteropImageInfo&, const org::TextureDescription&, std::string_view) { return {}; }
	void ReleaseImport(std::string_view) {}
	bool InBuiltGraph(std::string_view, Anchor) { return false; }
	void Rearm(std::string_view) {}
	void SetLateEpochHook(std::function<void()>) {}
	std::uint32_t SegmentOf(Anchor) { return 0; }
	bool IsUnusedSegment(std::uint32_t) { return false; }
	const SegmentNames* NamesOf(std::uint32_t) { return nullptr; }
	void OnEpochFailed(std::uint32_t, const std::string&, bool) {}
	void AddToFrame(std::vector<std::uint32_t>&) {}
	void SetTicketsAhead(org::GraphHost&) {}
	void Attach(org::GraphHost&) {}
}

#endif

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/FrameAnchors.h"

#	include "Globals.h"
#	include "RenderGraph/Frame/FrameDescription.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#	include "RenderGraph/Engine/Native/Native.h"
#	include "RenderGraph/Engine/Targets/EngineTargets.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/BuildManager.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/ExternalWork.h"
#	include "RenderGraph/FrameSequence.h"
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
#	include <chrono>
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
			Segment::AnchorDecalQueries,       // the decal occlusion tests, at the frame's start before Main::Draw
			Segment::AnchorEngineShadowClear,  // each shadow map slice's clear, before its draws (nominally here)
			Segment::PhaseFrameStart,          // the frame-start phase: submitted at the shadow view epoch
			Segment::ShadowView,
			Segment::SkyOcclusion,
			Segment::AnchorEarlyPrepass,      // Main::RenderShadowMaps returned
			Segment::DeclaredPreDepth,        // the declared pre-depth segment (SegmentsOn): its tap is EarlyPrepass's hook
			Segment::AnchorEngineFrameClear,  // the main depth's clear in Main::RenderDepth (and the frame's other clears)
			Segment::PhasePreDepth,           // the pre-depth phase: submitted at the Z-prepass
			Segment::ZPrepass,                // Main::RenderDepth
			Segment::FirstPersonDepth,
			Segment::AnchorEngineDepthCopy,  // Main::RenderDepth's end (and the later copies of the same depth)
			Segment::AnchorDepthDone,        // Main::RenderDepth returned, with the first-person depth
			Segment::AnchorEngineCubeClear,  // each reflection face's clear, before its draws (nominally here)
			Segment::Reflection,
			Segment::AnchorReflectionsPrepass,  // the cubemap render, nominally here (zero or more times a frame)
			Segment::AnchorSkyReflections,      // its faces' sky, once per face drawn
			Segment::LightCulling,              // Light Limit Fix's Prepass
			Segment::AnchorPrepass,             // after every feature's Prepass
			Segment::AnchorSky,                 // the main pass's sky batch, before Drawcall Limit Fix's colour epoch
			Segment::PhasePreMain,              // the pre-main phase: submitted at the main opaque epoch
			Segment::MainOpaque,
			Segment::AnchorDeferredPasses,     // EndDeferred, after the blended decals
			Segment::AnchorDeferredComposite,  // DeferredPasses, after the D3D11 SSGI and SSS
			Segment::AnchorPostComposite,      // DeferredPasses' end, after the composite
			Segment::AnchorEngineDraws,        // the water, before the forward epoch (nominally here: the shadow masks and the water mask run before, the blood after)
			Segment::PhaseDeferred,            // the deferred phase: submitted at the forward epoch
			Segment::Forward,
			Segment::AnchorEngineQueries,  // the sun's occlusion queries, after Main::RenderWorld's accumulator (nominally here: a second runs later in the frame)
			Segment::FirstPerson,
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
			Segment::AnchorOverlay,    // the overlay's ImGui draw, inside HDRDisplay's present chain
			Segment::AnchorHDROutput,  // HDRDisplay's output composite, after the overlay
			Segment::AnchorCapture,    // the screenshot capture, at the end of HDRDisplay's present chain
			Segment::PhasePost,        // the post phase: after the first-person epoch (which does not submit it), submitted at the present
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
		 * @brief The frame phases (native mode): the stretches of the frame between Drawcall Limit Fix's epochs. Each phase is one epoch whose
		 * graph holds the passes of its member anchors. Every run of a member in that stretch writes into the phase's one held execution, which
		 * is submitted when the frame reaches the next epoch (or the present). A run's phase is the stretch it runs in: the DCLF epoch submitted
		 * last this frame (PhaseOfGap). Membership starts from the census (kPhaseOrder) and is learned: a gated anchor that runs in a phase
		 * whose graph lacks it runs in its own epoch, and the phase's graph is built again with it.
		 */
		enum class Phase : std::uint8_t
		{
			FrameStart,  // before the shadow view: the reflection faces, the shadow clears (and every run of a frame without DCLF epochs)
			PreDepth,    // between the shadow view and the Z-prepass: the early prepass, the main depth's clear
			PreMain,     // between the Z-prepass and the main opaque epoch
			Deferred,    // between the main opaque and the forward epochs
			Post,        // after the forward epoch: placed after the first-person epoch, which does not submit it
			Count,
			None = Count,
		};
		constexpr std::uint32_t kPhases = static_cast<std::uint32_t>(Phase::Count);

		constexpr std::array<SegmentNames, kPhases> kPhaseNames{ {
			{ "CS phase: frame start", "CS phase: frame start inputs", "FramePhases::FrameStart / ", "cs.phase.frame-start" },
			{ "CS phase: pre-depth", "CS phase: pre-depth inputs", "FramePhases::PreDepth / ", "cs.phase.pre-depth" },
			{ "CS phase: pre-main", "CS phase: pre-main inputs", "FramePhases::PreMain / ", "cs.phase.pre-main" },
			{ "CS phase: deferred", "CS phase: deferred inputs", "FramePhases::Deferred / ", "cs.phase.deferred" },
			{ "CS phase: post", "CS phase: post inputs", "FramePhases::Post / ", "cs.phase.post" },
		} };

		std::uint32_t PhaseIndex(Phase a_phase) { return static_cast<std::uint32_t>(a_phase); }
		Segment PhaseSegment(Phase a_phase) { return static_cast<Segment>(static_cast<std::uint32_t>(Segment::PhaseFrameStart) + PhaseIndex(a_phase)); }
		std::uint32_t EpochOfPhase(Phase a_phase) { return RenderGraphRuntime::EpochOf(PhaseSegment(a_phase)); }

		/**
		 * @brief Each phase's anchors in the order they run there, from the frame sequence census (CS_ORG_FRAME_SEQUENCE, a world frame).
		 * It is the seed of the phase's membership, and the order of its graph's passes. An anchor not listed comes after the listed ones, in
		 * kTimeline's order. A run out of this order starts another execution of the phase.
		 */
		constexpr std::array<std::array<Anchor, 16>, kPhases> kPhaseOrder{ {
			{ Anchor::DecalQueries, Anchor::ReflectionsPrepass, Anchor::EngineCubeClear, Anchor::SkyReflections, Anchor::EngineShadowClear, Anchor::Count,
				Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count,
				Anchor::Count },
			{ Anchor::EarlyPrepass, Anchor::EngineFrameClear, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count,
				Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count },
			{ Anchor::EngineDepthCopy, Anchor::DepthDone, Anchor::EngineFrameClear, Anchor::EngineDraws, Anchor::Prepass, Anchor::Sky, Anchor::Count,
				Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count },
			{ Anchor::EngineDepthCopy, Anchor::DeferredPasses, Anchor::DeferredComposite, Anchor::PostComposite, Anchor::EngineFrameClear, Anchor::EngineDraws,
				Anchor::ImageSpace, Anchor::EngineCopy, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count, Anchor::Count,
				Anchor::Count },
			{ Anchor::EngineQueries, Anchor::EngineDraws, Anchor::VolumetricLighting, Anchor::PostProcessing, Anchor::Upscale, Anchor::ImageSpace,
				Anchor::EngineCopy, Anchor::EngineFrameClear, Anchor::Tonemap, Anchor::FrameGeneration, Anchor::MovieTiles, Anchor::Interface, Anchor::Present,
				Anchor::Overlay, Anchor::HDROutput, Anchor::Capture },
		} };

		/** @brief The anchors kPhaseOrder lists for a phase, a bit each: its first members. */
		std::uint32_t PhaseSeed(Phase a_phase)
		{
			std::uint32_t mask = 0;
			for (const Anchor anchor : kPhaseOrder[PhaseIndex(a_phase)])
				if (anchor != Anchor::Count)
					mask |= 1u << static_cast<std::uint32_t>(anchor);
			return mask;
		}

		/**
		 * @brief The phase of the stretch after DCLF epoch a_gap (a Segment value; UINT32_MAX before any this frame). The forward epoch's
		 * stretch belongs to the post phase: its runs (the sun queries) go after the first-person epoch.
		 */
		Phase PhaseOfGap(std::uint32_t a_gap)
		{
			switch (static_cast<Segment>(a_gap)) {
			case Segment::ShadowView:
			case Segment::SkyOcclusion:
				return Phase::PreDepth;
			case Segment::ZPrepass:
			case Segment::FirstPersonDepth:
			case Segment::Reflection:
				return Phase::PreMain;
			case Segment::MainOpaque:
				return Phase::Deferred;
			case Segment::Forward:
			case Segment::FirstPerson:
				return Phase::Post;
			default:
				return Phase::FrameStart;
			}
		}

		/** @brief Where anchor a_index's passes and runs sit in a_phase: its place in kPhaseOrder, or after those in kTimeline's order. */
		std::uint32_t PhaseRank(Phase a_phase, std::uint32_t a_index)
		{
			const auto& order = kPhaseOrder[PhaseIndex(a_phase)];
			for (std::uint32_t i = 0; i < order.size(); ++i)
				if (order[i] != Anchor::Count && static_cast<std::uint32_t>(order[i]) == a_index)
					return i;
			return static_cast<std::uint32_t>(order.size()) + RankOf(SegmentOf(static_cast<Anchor>(a_index)));
		}

		/**
		 * @brief The declared frame's segments (SegmentsOn). Each is one epoch whose graph holds the passes of the anchors it lists,
		 * each anchor's by order key, and its tap is its last anchor's hook, where its one ticket is committed. Stage 1 has the
		 * pre-depth segment alone, whose one anchor is EarlyPrepass.
		 */
		constexpr std::uint32_t kDeclared = RenderGraph::Frame::kSegments;
		struct DeclaredInfo
		{
			Segment segment;
			std::uint32_t members;  // the anchors whose passes it holds, a bit each
			Anchor tap;             // the anchor whose hook ends it
			SegmentNames names;
		};
		constexpr std::array<DeclaredInfo, kDeclared> kDeclaredInfo{ {
			{ Segment::DeclaredPreDepth, 1u << static_cast<std::uint32_t>(Anchor::EarlyPrepass), Anchor::EarlyPrepass,
				{ "CS segment: pre-depth", "CS segment: pre-depth inputs", "FrameSegments::PreDepth / ", "cs.segment.pre-depth" } },
		} };

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
			std::string error;                                               // why it failed or was dropped
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
			std::condition_variable ready;
			std::vector<std::shared_ptr<SegmentRecording>> entries;
			RenderGraph::Frame::SegmentFields last{};  // the fields of the last description asked for, for the reason a new one logs
			bool any = false;
			// The segment's version generation (SegmentFields::versions): moved when a kept recording binds a version that is no
			// longer current. Render owner.
			std::uint64_t versionGeneration = 0;
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
			// Phases: the anchors whose passes are all gated, which can run in a phase's epoch (phases on). A bit per anchor.
			std::uint32_t merged = 0;
			// Each phase's members in its graph: the learned members that are merged.
			std::array<std::uint32_t, kPhases> members{};
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

		/** @brief Phases: the execution a phase holds now, which FlushRegions submits. Render owner. */
		struct Held
		{
			Phase phase = Phase::None;
			std::uint32_t epoch = 0;
			std::shared_ptr<const Snapshot> built;  // the phase graph's snapshot when the hold began: the graph the held ticket is of
			std::uint32_t anchors = 0;              // the anchors whose runs committed into it, a bit each
			std::uint32_t lastRank = 0;             // the rank of the last of them (PhaseRank)
			// The clients whose commits wrote into the run, with the anchor of each commit.
			std::vector<std::pair<std::uint32_t, std::shared_ptr<const ClientDesc>>> committed;
			bool sharedConstants = false;    // CS's shared constants were written into the run
			bool late = false;               // a late anchor (AnchorInfo::late) committed
			std::uint32_t runs = 0;          // the anchor runs it holds
			std::uint64_t externalWork = 0;  // ExternalWork's submissions when the hold began
		};

		/** @brief Phases: what the executions held and why they were submitted, since the last report (LogRunStats). Render owner. */
		struct PhaseStats
		{
			std::uint64_t executions = 0;                               // phase executions submitted, once each however many runs joined
			std::uint64_t runs = 0;                                     // anchor runs they held
			std::uint64_t lost = 0;                                     // held runs a build gave back before their submission
			std::uint64_t failed = 0;                                   // held executions whose submission failed (not in executions)
			std::vector<std::pair<const char*, std::uint64_t>> causes;  // why each was submitted, by static string
			void Count(const char* a_why)
			{
				for (auto& [why, count] : causes)
					if (std::string_view(why) == a_why) {
						++count;
						return;
					}
				causes.emplace_back(a_why, 1);
			}
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
			// Phases: whether they have epochs (AddToFrame), and each phase graph's snapshots, as for the anchors above.
			std::array<bool, kPhases> phaseEpochs{};
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kPhases> phaseSnapshots{};
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kPhases> phasePrevious{};
			// Declared segments: whether each has an epoch (AddToFrame), the anchors they hold (which then have no epoch of their own),
			// each segment graph's snapshots as for the anchors above, and each segment's recordings.
			std::array<bool, kDeclared> declaredEpochs{};
			std::uint32_t declaredMembers = 0;
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kDeclared> declaredSnapshots{};
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kDeclared> declaredPrevious{};
			std::array<SegmentRecordings, kDeclared> declaredRecordings;
			// Each phase's members, a bit per anchor: kPhaseOrder's at first, and every gated anchor seen running in it since (Run). Only
			// ever added to. Publish copies them into the snapshot.
			std::array<std::uint32_t, kPhases> learned{};
			// The DCLF epoch submitted last this frame (a Segment value, UINT32_MAX for none), and the host seal it was noted under:
			// the stretch of the frame a run is in (PhaseOfGap).
			std::uint32_t gap = UINT32_MAX;
			std::uint64_t gapSeal = 0;
			std::optional<Held> held;
			std::atomic<bool> holding{ false };  // held has a value: FlushRegions' check without the mutex
			bool flushing = false;               // FlushRegions is submitting the held execution
			bool committing = false;             // a run's commits are writing into the held run
			PhaseStats phaseStats;
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

		/** @brief The phase of a phase epoch's graph, or None. */
		Phase PhaseOfEpoch(std::uint32_t a_epoch)
		{
			for (std::uint32_t p = 0; p < kPhases; ++p)
				if (Anchors().phaseEpochs[p] && a_epoch == EpochOfPhase(static_cast<Phase>(p)))
					return static_cast<Phase>(p);
			return Phase::None;
		}

		/** @brief The declared segment of a segment epoch's graph, or kDeclared. */
		std::uint32_t DeclaredOfEpoch(std::uint32_t a_epoch)
		{
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if (Anchors().declaredEpochs[d] && a_epoch == EpochOfDeclared(d))
					return d;
			return kDeclared;
		}

		/** @brief The declared segment whose epoch holds anchor a_index's passes, or kDeclared when the anchor has its own epoch. */
		std::uint32_t DeclaredOfAnchor(const State& a_state, std::uint32_t a_index)
		{
			if (!(a_state.declaredMembers >> a_index & 1u))
				return kDeclared;
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if (a_state.declaredEpochs[d] && (kDeclaredInfo[d].members >> a_index & 1u))
					return d;
			return kDeclared;
		}

		/**
		 * @brief The snapshot the anchor's own graph was built from. Its own graph always holds its passes: a merged anchor that runs in a
		 * phase whose graph lacks it runs there.
		 */
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

		/** @brief The epochs whose graphs hold (or will hold) the passes of the anchors in a_mask: each one's own, and the phases they are in. */
		std::vector<std::uint32_t> EpochsOf(const State& a_state, std::uint32_t a_mask)
		{
			std::vector<std::uint32_t> epochs;
			std::uint32_t anchors = 0;
			for (std::uint32_t i = 0; i < kAnchors; ++i) {
				if (!(a_mask >> i & 1u) || !a_state.reserved[i])
					continue;
				// A declared segment's member has no epoch of its own: its passes are in the segment's.
				if (const auto d = DeclaredOfAnchor(a_state, i); d < kDeclared) {
					if (std::ranges::find(epochs, EpochOfDeclared(d)) == epochs.end())
						epochs.push_back(EpochOfDeclared(d));
					continue;
				}
				epochs.push_back(EpochOfAnchor(static_cast<Anchor>(i)));
				anchors |= 1u << i;
			}
			for (std::uint32_t p = 0; p < kPhases; ++p)
				if (a_state.phaseEpochs[p] && (a_state.learned[p] & anchors))
					epochs.push_back(EpochOfPhase(static_cast<Phase>(p)));
			return epochs;
		}

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
			// Phases: an anchor whose passes are all gated can run in a phase's epoch. A one-shot pass is never gated: it would run at
			// every execution of the phase.
			if (RegionsOn()) {
				for (std::uint32_t i = 0; i < kAnchors; ++i) {
					bool any = false;
					bool gated = true;
					for (const auto& scheduled : snapshot->passes)
						if (IndexOf(scheduled.pass->anchor) == i) {
							any = true;
							gated = gated && scheduled.pass->gated && !scheduled.pass->once;
						}
					if (any && gated)
						snapshot->merged |= 1u << i;
				}
				for (std::uint32_t p = 0; p < kPhases; ++p)
					if (a_state.phaseEpochs[p])
						snapshot->members[p] = a_state.learned[p] & snapshot->merged;
			}
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
						if (a_state.declaredEpochs[d] && (kDeclaredInfo[d].members >> i & 1u) &&
							builtFrom(a_state.declaredSnapshots[d].load(std::memory_order_acquire)) < a_retired.serial)
							return false;
					// Phases: a phase's graph may hold the old passes too.
					for (std::uint32_t p = 0; p < kPhases; ++p)
						if (a_state.phaseEpochs[p] && (a_state.learned[p] >> i & 1u) &&
							builtFrom(a_state.phaseSnapshots[p].load(std::memory_order_acquire)) < a_retired.serial)
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
		 * @brief The anchors' passes in each graph: an anchor epoch's graph takes its anchor's passes, a phase epoch's graph the passes of
		 * its members (Snapshot::members); each registers the clients' resources and the engine imports. Every other epoch's graph
		 * gets nothing from it.
		 */
		/** @brief A declared segment's clients in a snapshot, in registration order: the order of the description's clientsOn bits. */
		std::vector<std::shared_ptr<const ClientDesc>> SegmentClients(const Snapshot& a_snapshot, std::uint32_t a_declared)
		{
			std::vector<std::shared_ptr<const ClientDesc>> clients;
			for (const auto& client : a_snapshot.clients)
				if (AnchorMask(*client) & kDeclaredInfo[a_declared].members)
					clients.push_back(client);
			return clients;
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
				const Phase phase = anchor == Anchor::Count ? PhaseOfEpoch(epoch) : Phase::None;
				const std::uint32_t declared = anchor == Anchor::Count && phase == Phase::None ? DeclaredOfEpoch(epoch) : kDeclared;
				if (anchor == Anchor::Count && phase == Phase::None && declared == kDeclared)
					return;
				// What the run that submits this graph reads (ClientDesc::executed, one-shot passes): the graph holds this snapshot's passes.
				// Recorded when the graph is installed: at its attach for a build the build manager makes (on its own thread, and possibly
				// dropped), at once for one the host makes on the owner thread. A failed managed build drops its clients (DropFailedClients).
				if (declared != kDeclared) {
					// A declared segment's graph: what it was built from is the segment's, and each member anchor's.
					const auto commit = [declared, built = snapshot] {
						auto& anchors = Anchors();
						anchors.declaredPrevious[declared].store(anchors.declaredSnapshots[declared].load(std::memory_order_acquire), std::memory_order_release);
						anchors.declaredSnapshots[declared].store(built, std::memory_order_release);
						for (std::uint32_t i = 0; i < kAnchors; ++i)
							if (kDeclaredInfo[declared].members >> i & 1u) {
								anchors.built[i].store(built->serial, std::memory_order_release);
								anchors.previousSnapshots[i].store(anchors.builtSnapshots[i].load(std::memory_order_acquire), std::memory_order_release);
								anchors.builtSnapshots[i].store(built, std::memory_order_release);
							}
					};
					const auto onFailed = [declared, built = snapshot](const std::string& a_error) {
						DropFailedClients(kDeclaredInfo[declared].members, kDeclaredInfo[declared].names.label, built, a_error);
					};
					if (!RenderGraph::Builds::Defer(commit, onFailed))
						commit();
				} else if (anchor != Anchor::Count) {
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
				} else {
					const auto r = PhaseIndex(phase);
					const auto commit = [r, built = snapshot] {
						auto& anchors = Anchors();
						anchors.phasePrevious[r].store(anchors.phaseSnapshots[r].load(std::memory_order_acquire), std::memory_order_release);
						anchors.phaseSnapshots[r].store(built, std::memory_order_release);
					};
					const auto onFailed = [r, built = snapshot](const std::string& a_error) {
						DropFailedClients(built->members[r], kPhaseNames[r].label, built, a_error);
					};
					if (!RenderGraph::Builds::Defer(commit, onFailed))
						commit();
				}
				// Each import's slot is the id's resolver, so a declaration by id follows its versions too.
				for (const auto& [id, slot] : snapshot->imports)
					if (const org::ResourceIdentifier key(id); !a_graph._resolverMap.contains(key))
						a_graph.RegisterResolver(key, slot);
				for (const auto& client : snapshot->clients)
					if (client->registerResources)
						client->registerResources(a_graph);
				if (UsesSharedConstantsIn(snapshot->clients, AnchorsIn(anchor, phase, declared)))
					SharedConstants::Register(a_graph);
			}

			void GatherStructuralPasses(org::RenderGraph& a_graph, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				if (!snapshot)
					return;
				const std::uint32_t epoch = a_graph.PassEpochFilter();
				const Anchor anchor = AnchorOfEpoch(epoch);
				const Phase phase = anchor == Anchor::Count ? PhaseOfEpoch(epoch) : Phase::None;
				const std::uint32_t declared = anchor == Anchor::Count && phase == Phase::None ? DeclaredOfEpoch(epoch) : kDeclared;
				const std::uint32_t anchors = AnchorsIn(anchor, phase, declared);
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
					if (anchors >> IndexOf(scheduled.pass->anchor) & 1u)
						passes.push_back(scheduled.pass);
				// A phase's graph runs its anchors in the phase's order (a held execution only ever holds runs in that order), each
				// anchor's passes by their order keys.
				if (phase != Phase::None)
					std::ranges::stable_sort(passes, [phase](const PassDesc* a, const PassDesc* b) {
						return PhaseRank(phase, IndexOf(a->anchor)) < PhaseRank(phase, IndexOf(b->anchor));
					});
				for (const auto* scheduled : passes) {
					const auto& pass = *scheduled;
					auto made = pass.make ? pass.make() : nullptr;
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
			/**
			 * @brief The anchors whose passes this snapshot puts in the graph: an anchor's own always (a merged anchor's runs fall back to it in
			 * a phase whose graph lacks it), a phase's members, a declared segment's members.
			 */
			std::uint32_t AnchorsIn(Anchor a_anchor, Phase a_phase, std::uint32_t a_declared) const
			{
				if (a_anchor != Anchor::Count)
					return 1u << IndexOf(a_anchor);
				if (a_phase != Phase::None)
					return snapshot->members[PhaseIndex(a_phase)];
				if (a_declared != kDeclared)
					return kDeclaredInfo[a_declared].members;
				return 0;
			}

			/**
			 * @brief A declared segment's graph: its members' passes, each anchor's by order key, all recorded ahead on the host's thread.
			 * Each pass is off in a recording whose description turns its client off (SetSkipWhen): it records only its barriers then.
			 * A pass marked atExecution is recorded ahead too. For a gated copy or clear that only means it runs on every submission of
			 * a description that has its client on. A pass that is recorded at execution for any other reason cannot be in a segment
			 * yet: the build logs it as an error.
			 */
			void GatherDeclared(org::RenderGraph& a_graph, std::uint32_t a_declared, std::uint32_t a_epoch, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) const
			{
				const std::uint32_t anchors = kDeclaredInfo[a_declared].members;
				if (UsesSharedConstantsIn(snapshot->clients, anchors))
					if (auto upload = SharedConstants::MakeUploadPass())
						a_out.push_back(std::move(org::RenderGraph::ExternalPassDesc::Copy("cs.shared.upload", std::move(upload))
								.PreferQueue(org::QueueKind::Graphics)
								.Epoch(a_epoch)));
				const auto clients = SegmentClients(*snapshot, a_declared);
				for (const auto& scheduled : snapshot->passes) {
					if (!(anchors >> IndexOf(scheduled.pass->anchor) & 1u))
						continue;
					const auto& pass = *scheduled.pass;
					auto made = pass.make ? pass.make() : nullptr;
					if (!made)
						continue;
					if (pass.atExecution && !pass.gated)
						logger::error("[ORG] frame segments: {} is recorded at execution and not gated; in the {} it is recorded ahead, which it may not support",
							pass.name, kDeclaredInfo[a_declared].names.label);
					const auto found = std::ranges::find(clients, scheduled.client);
					const auto bit = static_cast<std::uint32_t>(found - clients.begin());
					made->SetSkipWhen([a_declared, bit](const org::IHostExecutionData* a_data) {
						const auto* data = a_data ? a_data->Get<SegmentHostData>() : nullptr;
						return data && data->segment == a_declared && bit < 32 && !(data->fields.clientsOn >> bit & 1u);
					});
					auto desc = pass.kind == PassKind::Compute ? org::RenderGraph::ExternalPassDesc::Compute(pass.name, std::move(made)) :
					            pass.kind == PassKind::Copy    ? org::RenderGraph::ExternalPassDesc::Copy(pass.name, std::move(made)) :
					                                             org::RenderGraph::ExternalPassDesc::Render(pass.name, std::move(made));
					if (pass.queue == PassQueue::Compute)
						desc.PreferQueue(org::QueueKind::Compute);
					else if (pass.kind != PassKind::Render)
						desc.PreferQueue(org::QueueKind::Graphics);
					a_out.push_back(std::move(desc.Epoch(a_epoch)));
				}
				(void)a_graph;
			}

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
		const std::uint32_t phases = static_cast<std::uint32_t>(Segment::PhaseFrameStart);
		if (a_segment >= phases && a_segment - phases < kPhases)
			return &kPhaseNames[a_segment - phases];
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
		const std::uint32_t phases = static_cast<std::uint32_t>(Segment::PhaseFrameStart);
		if (a_segment >= phases && a_segment - phases < kPhases) {
			const auto& state = Anchors();
			return !state.frozen || !state.phaseEpochs[a_segment - phases];
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
			if (!HasEpoch(pass.anchor) || pass.name.empty() || !pass.make || std::ranges::find(names, pass.name) != names.end()) {
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

		// Regions: submits the held execution (defined with Run).
		void SubmitHeld(State& a_state, const char* a_why);

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
		// A region's execution held by the last owner (its present submits it, so this is a safety net) goes before this thread's work.
		if (auto& state = Anchors(); state.held)
			SubmitHeld(state, "render ownership moved");
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

	bool RegionsOn()
	{
		// Native mode groups anchors into region subgraphs; guest mode keeps one epoch per anchor in DXVK's stream.
		static const bool on = [] {
			if (!RenderGraph::Engine::Native::Requested())
				return false;
			// Off by default (CS_ORG_PHASES=1 turns them on): with kept live recordings, per-anchor epochs run faster than phase
			// epochs (perf-taa 73 against 52.5 fps). A phase execution records every gated pass of its larger graph again, and its
			// admission covers more resources.
			if (Switches::Number("CS_ORG_PHASES", 0) == 0) {
				logger::info("[ORG] frame phases off (CS_ORG_PHASES=1 turns them on): every anchor keeps its own epoch");
				return false;
			}
			if (!RenderGraphRuntime::EpochsEnabled() || Switches::Equals("CS_ORG_ASYNC_EPOCHS", "0")) {
				logger::warn("[ORG] frame phases need epochs and async epochs; every anchor keeps its own epoch");
				return false;
			}
			return true;
		}();
		return on;
	}

	namespace
	{
		/** @brief Gives the held execution's ticket back unsubmitted: what its runs wrote is dropped (the graph stopped). */
		void DropHeld(State& a_state)
		{
			if (!a_state.held)
				return;
			const auto epoch = a_state.held->epoch;
			a_state.held.reset();
			a_state.holding.store(false, std::memory_order_release);
			if (!a_state.host)
				return;
			try {
				(void)a_state.host->ReleaseHeldEpochTicket(epoch);
			} catch (const std::exception& e) {
				logger::error("[ORG] frame phases: a held run could not be given back: {}", e.what());
			}
		}

		/** @brief The graph failed and stopped (a_name's epoch said so): every client returns to its D3D11 path. */
		void StopAll(State& a_state, const char* a_name)
		{
			logger::error("[ORG] frame anchors: the {} epoch failed; {} clients return to D3D11", a_name, a_state.clients.size());
			DropHeld(a_state);
			auto failed = std::move(a_state.clients);
			a_state.clients.clear();
			a_state.retired.clear();
			Publish(a_state);
			for (const auto& client : failed)
				if (client.desc->onFailed)
					client.desc->onFailed();
		}

		/**
		 * @brief Submits the held execution: every run it holds reaches the GPU now, ahead of the work that asked for it (a_why, counted
		 * in the phase statistics). Render owner, under the anchors' mutex.
		 */
		void SubmitHeld(State& a_state, const char* a_why)
		{
			if (!a_state.held || a_state.flushing)
				return;
			if (a_state.committing) {
				// A commit asked for GPU work while it wrote into the held run: the execution is submitted after the commits instead.
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::warn("[ORG] frame phases: GPU work was submitted from inside a commit ({}); the held execution goes after it (logged once)", a_why);
				return;
			}
			Held held = std::move(*a_state.held);
			a_state.held.reset();
			a_state.holding.store(false, std::memory_order_release);
			auto& runtime = RenderGraphRuntime::Get();
			if (!runtime.IsActive() || !a_state.host)
				return;
			const char* name = kPhaseNames[PhaseIndex(held.phase)].label;
			// A build of the region's epoch gave the ticket back (the build points submit first, so this should not happen): what its
			// runs wrote is gone, and a new ticket would run their passes with nothing opened.
			if (!a_state.host->HoldsEpochTicket(held.epoch)) {
				++a_state.phaseStats.lost;
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::error(
						"[ORG] frame phases: a build gave the {} execution's held run back before its submission; its {} anchor runs are lost "
						"(logged once)",
						name, held.runs);
				return;
			}
			RenderGraphRuntime::EpochResult result = RenderGraphRuntime::EpochResult::Stopped;
			a_state.flushing = true;
			try {
				result = runtime.TryExecuteEpoch(PhaseSegment(held.phase));
			} catch (...) {
				a_state.flushing = false;
				throw;
			}
			a_state.flushing = false;
			if (result != RenderGraphRuntime::EpochResult::Submitted) {
				// It did not reach the GPU: counted as failed, not as an execution.
				++a_state.phaseStats.failed;
				// Nothing was submitted; a ticket still held goes back.
				if (a_state.host->HoldsEpochTicket(held.epoch)) {
					try {
						(void)a_state.host->ReleaseHeldEpochTicket(held.epoch);
					} catch (const std::exception& e) {
						logger::error("[ORG] frame phases: a held run could not be given back: {}", e.what());
					}
				}
				// Only the region's epoch failed: its clients are dropped at the next poll (OnEpochFailed). Otherwise the graph stopped.
				if (result != RenderGraphRuntime::EpochResult::EpochFailed)
					StopAll(a_state, name);
				return;
			}
			// An execution counts once, with all the runs that joined it.
			++a_state.phaseStats.executions;
			a_state.phaseStats.runs += held.runs;
			a_state.phaseStats.Count(a_why);
			// An epoch must not be the last GPU work before a third-party Vulkan consumer (render-graph.md, "Hazard: external Vulkan
			// work directly after a graph epoch").
			if (held.late && a_state.lateHook)
				a_state.lateHook();
			if (held.sharedConstants)
				SharedConstants::CheckAfterRun();
			PruneRetired(a_state);
		}

		/**
		 * @brief Why a run of anchor a_index cannot write into the held execution (null: it can). Its passes must come after every
		 * pass the execution's runs opened (the phase's graph runs anchors in its order, each one's passes by order key), and no
		 * latch region or gate entry may be written twice.
		 */
		/** @brief A lasting copy of a_text, for PhaseStats' causes (which keep pointers). Render owner. */
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

		const char* JoinRefusal(const Held& a_held, std::uint32_t a_index, const std::vector<RunClient>& a_clients, bool a_sharedConstants)
		{
			const bool repeat = (a_held.anchors >> a_index & 1u) != 0;
			// A new anchor comes after the last one; a repeat only directly after itself.
			const std::uint32_t rank = PhaseRank(a_held.phase, a_index);
			if (repeat ? rank != a_held.lastRank : rank <= a_held.lastRank)
				return Interned(fmt::format("an anchor out of the phase's order ({})", kInfo[a_index].name));
			const auto committed = [&](const std::shared_ptr<const ClientDesc>& a_desc) {
				return std::ranges::any_of(a_held.committed, [&](const auto& a_entry) { return a_entry.second == a_desc; });
			};
			if (repeat)
				for (const auto& entry : a_held.committed)
					if (entry.first == a_index &&
						std::ranges::none_of(a_clients, [&](const RunClient& a_client) { return a_client.active && a_client.desc == entry.second; }))
						return "an anchor run again with other clients";
			for (const auto& client : a_clients) {
				if (!client.active)
					continue;
				const bool before = committed(client.desc);
				if (repeat && !before)
					return "an anchor run again with other clients";
				if (before && !(client.desc->joins && client.desc->joins()))
					return Interned(fmt::format("a client's second commit ({} at {})", client.desc->id, kInfo[a_index].name));
			}
			if (a_sharedConstants && a_held.sharedConstants && !SharedConstants::CommittedMatches())
				return "the shared constants changed";
			if (RenderGraph::ExternalWork::Submissions() != a_held.externalWork) {
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::warn(
						"[ORG] frame phases: external work (RenderGraph::ExternalWork) was submitted while a phase held anchor runs; it ran "
						"ahead of them (logged once)");
				return "external work";
			}
			return nullptr;
		}

		/** @brief The phase of the frame's stretch now: after the DCLF epoch submitted last since the host's last seal. Render owner. */
		Phase CurrentPhase(const State& a_state)
		{
			if (!a_state.host || a_state.gapSeal != a_state.host->Seals())
				return Phase::FrameStart;
			return PhaseOfGap(a_state.gap);
		}

		/**
		 * @brief A merged anchor ran in a phase whose members lack it: it joins them, and the phase's graph is built again (off the render
		 * thread, attached at a frame boundary). Until then its runs there take its own epoch. Render owner, under the anchors' mutex.
		 */
		void LearnMember(State& a_state, Phase a_phase, std::uint32_t a_index)
		{
			const auto p = PhaseIndex(a_phase);
			a_state.learned[p] |= 1u << a_index;
			logger::info("[ORG] frame phases: {} ran in the {} phase; its graph is built again with it (until then the anchor's runs there take its own epoch)",
				kInfo[a_index].name, kPhaseNames[p].label);
			Publish(a_state);
			RenderGraph::Builds::Request(EpochOfPhase(a_phase), fmt::format("frame phases: {} joins {}", kInfo[a_index].name, kPhaseNames[p].label));
		}

		/**
		 * @brief A run of a merged anchor (its passes in a_phase's built graph): its commits write into the phase's held run, which is
		 * submitted later (SubmitHeld). Returns Ran once they did: the passes reach the GPU before any later GPU work, so the clients'
		 * executed callbacks run now.
		 */
		RunResult Capture(State& a_state, const Point& a_point, Phase a_phase, const std::shared_ptr<const Snapshot>& a_built,
			const std::vector<RunClient>& a_clients)
		{
			const std::uint32_t index = IndexOf(a_point.anchor);
			const char* name = kPhaseNames[PhaseIndex(a_phase)].label;
			const bool sharedConstants = std::ranges::any_of(a_clients, [&](const RunClient& a_client) { return a_client.desc->sharedConstants; });
			// The held execution takes this run, or is submitted first.
			if (a_state.held && a_state.held->phase != a_phase)
				SubmitHeld(a_state, "another phase");
			if (a_state.held && a_state.held->built != a_built)
				SubmitHeld(a_state, "a new graph of the phase");
			if (a_state.held)
				if (const char* why = JoinRefusal(*a_state.held, index, a_clients, sharedConstants))
					SubmitHeld(a_state, why);
			if (!RenderGraphRuntime::Get().IsActive())
				return { RunStatus::Failed };
			auto& host = *a_state.host;
			const std::uint32_t epoch = EpochOfPhase(a_phase);
			if (!a_state.held) {
				try {
					(void)host.HoldEpochTicket(epoch);
				} catch (const org::GraphHost::EpochFailedError& e) {
					// Only the region's epoch failed: its clients are dropped at the next poll (OnEpochFailed).
					static std::atomic_flag logged;
					if (!logged.test_and_set())
						logger::error("[ORG] frame phases: the {} epoch did not run, its graph failed: {} (logged once)", name, e.what());
					return { RunStatus::Failed };
				} catch (const std::exception& e) {
					logger::error("[ORG] frame phases: the {} execution could not take its run: {}", name, e.what());
					StopAll(a_state, name);
					return { RunStatus::Failed };
				}
				a_state.held.emplace();
				auto& held = *a_state.held;
				held.phase = a_phase;
				held.epoch = epoch;
				held.built = a_built;
				held.externalWork = RenderGraph::ExternalWork::Submissions();
				a_state.holding.store(true, std::memory_order_release);
				// Every gate of the graph's passes closed first: the execution does work only where its runs' commits open it. That
				// includes the gates of registrations whose passes the graph still holds after they left (RetiredGates).
				host.UseHeldRun(epoch);
				const std::uint32_t merged = a_built->members[PhaseIndex(a_phase)];
				for (const auto& client : a_built->clients)
					if (AnchorMask(*client) & merged)
						for (const auto& gate : client->gates)
							if (gate)
								gate->Close();
				for (const auto& retired : a_state.retired)
					if ((retired.mask & merged) && a_built->serial < retired.serial)
						for (const auto& gate : retired.gates)
							gate->Close();
				host.EndHeldRun();
			}
			// The commits write into the held run. Nothing submits the held execution meanwhile (SubmitHeld waits for them), and the
			// held state is read again after them: a commit that stopped the graph dropped it.
			const bool writeShared = sharedConstants && !a_state.held->sharedConstants;
			std::vector<std::shared_ptr<const ClientDesc>> committed;
			{
				struct Committing
				{
					State& state;
					org::GraphHost& host;
					~Committing()
					{
						host.EndHeldRun();
						state.committing = false;
					}
				} committing{ a_state, host };
				a_state.committing = true;
				host.UseHeldRun(epoch);
				RenderGraph::RunShapes::BeginRun();
				// The upload of CS's shared constants runs once per execution: the first run writes them, and a later run joins only while
				// they are unchanged (JoinRefusal).
				if (writeShared)
					SharedConstants::Commit();
				for (const auto& client : a_clients)
					if (client.active && client.desc->commit) {
						client.desc->commit(a_point);
						committed.push_back(client.desc);
					}
			}
			if (!a_state.held)
				return { RunStatus::Failed };
			auto& held = *a_state.held;
			held.sharedConstants = held.sharedConstants || writeShared;
			for (auto& desc : committed)
				held.committed.emplace_back(index, std::move(desc));
			held.anchors |= 1u << index;
			held.lastRank = PhaseRank(a_phase, index);
			held.late = held.late || kInfo[index].late;
			++held.runs;
			// The run is counted, and the peak reported (LogRunStats).
			auto& runs = RunsSinceSeal(a_state, index);
			++runs;
			if (runs > a_state.peakRuns[index].load(std::memory_order_relaxed))
				a_state.peakRuns[index].store(runs, std::memory_order_relaxed);
			if (RenderGraph::RunShapes::On()) {
				std::uint64_t active = 0;
				for (const auto& client : a_clients)
					if (client.active)
						active += reinterpret_cast<std::uintptr_t>(client.desc.get()) * 0x9E3779B97F4A7C15ull;
				RenderGraph::RunShapes::EndRun(index, kInfo[index].name, a_point.input, a_point.output, active);
			}
			NoteSequenceRun(a_point, true, a_clients);
			// Last: an executed callback may register, refresh or rearm. A merged anchor has no one-shot pass (Snapshot::merged).
			const std::uint64_t built = a_built->serial;
			for (const auto& client : a_clients)
				if (client.active && client.desc->executed && client.serial <= built)
					client.desc->executed(a_point);
			return { RunStatus::Ran };
		}
	}

	void FlushRegions(const char* a_why, std::uint32_t a_segment)
	{
		auto& state = Anchors();
		if (!state.holding.load(std::memory_order_acquire) || !IsRenderOwner())
			return;
		auto lock = Guard();
		if (!state.held || state.flushing)
			return;
		if (a_segment != UINT32_MAX) {
			const auto held = static_cast<std::uint32_t>(PhaseSegment(state.held->phase));
			if (a_segment == held)
				return;
			// A DCLF epoch that comes before the held phase in the frame (the first-person epoch, for the post phase's sun queries) runs
			// first: the phase's work goes after it.
			if (RenderGraphRuntime::IsDclfSegment(static_cast<Segment>(a_segment)) && RankOf(held) > RankOf(a_segment))
				return;
		}
		SubmitHeld(state, a_why);
	}

	void NoteEpochSubmitted(std::uint32_t a_segment)
	{
		if (!RegionsOn() || !RenderGraphRuntime::IsDclfSegment(static_cast<Segment>(a_segment)) || !IsRenderOwner())
			return;
		auto lock = Guard();
		auto& state = Anchors();
		if (!state.host)
			return;
		state.gap = a_segment;
		state.gapSeal = state.host->Seals();
	}


	namespace
	{
		using RenderGraph::Frame::SegmentFields;
		using RenderGraph::Frame::SegmentId;
		namespace Driver = RenderGraph::Frame::Driver;

		/** @brief A declared segment's fields for a frame: its built graph, the host's versions, and its clients' declared predicates. */
		SegmentFields DescribeDeclared(State& a_state, std::uint32_t a_declared, bool a_declaredByKind)
		{
			SegmentFields fields;
			fields.declared = a_declaredByKind;
			if (!a_declaredByKind || !a_state.host)
				return fields;
			const auto built = a_state.declaredSnapshots[a_declared].load(std::memory_order_acquire);
			if (!built)
				return fields;  // graph 0: not built yet
			fields.graph = a_state.host->BuildGeneration(EpochOfDeclared(a_declared));
			fields.versions = a_state.declaredRecordings[a_declared].versionGeneration;
			const auto clients = SegmentClients(*built, a_declared);
			fields.clients = static_cast<std::uint32_t>(clients.size());
			for (std::size_t i = 0; i < clients.size() && i < 32; ++i)
				if (!clients[i]->declared || clients[i]->declared())
					fields.clientsOn |= 1u << i;
			return fields;
		}

		/**
		 * @brief The recording of a declared segment for a_fields: the one kept for them, or a new request to the host's thread (logged
		 * with the fields that changed since the last request). A failed or dropped one is asked for again. Render owner.
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
						recordings.entries.erase(it);
						break;
					}
					kept.lastFrame = a_frame;
					return *it;
				}
				reason = recordings.any ? RenderGraph::Frame::DiffFields(recordings.last, a_fields) : std::string("the first description");
				if (reason.empty()) {
					// The description recorded for another slot already, or a recording of it that failed.
					const bool other = std::ranges::any_of(recordings.entries, [&](const auto& a_entry) { return a_entry->hash == hash && a_entry->slot != a_slot; });
					reason = other ? fmt::format("the same description, for slot {}", a_slot) : std::string("the same description, its last recording failed or was dropped");
				}
				recordings.last = a_fields;
				recordings.any = true;
				entry->hash = hash;
				entry->slot = a_slot;
				entry->fields = a_fields;
				entry->lastFrame = a_frame;
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
			auto data = std::make_shared<SegmentHostData>();
			data->segment = a_declared;
			data->fields = a_fields;
			const bool posted = a_state.host->RequestEpochRecording(EpochOfDeclared(a_declared), std::move(data),
				[&recordings, entry](std::shared_ptr<const org::GraphHost::EpochRecording> a_recording, std::exception_ptr a_error) {
					std::string error;
					if (a_error) {
						try {
							std::rethrow_exception(a_error);
						} catch (const std::exception& e) {
							error = e.what();
						} catch (...) {
							error = "unknown error";
						}
					}
					{
						const std::lock_guard lock(recordings.mutex);
						entry->recording = std::move(a_recording);
						entry->error = std::move(error);
						entry->done = true;
					}
					recordings.ready.notify_all();
				},
				a_slot);
			if (!posted) {
				const std::lock_guard lock(recordings.mutex);
				entry->error = "the host takes no recording requests (async epochs are off)";
				entry->done = true;
			}
			return entry;
		}

		/**
		 * @brief Whether the finished recording kept for a_fields and a_slot binds a version that is no longer current: a slot of the
		 * segment's graph published a new one since. Then the segment's version generation moves, which makes a new description.
		 * Render owner.
		 */
		bool StaleVersions(State& a_state, std::uint32_t a_declared, const SegmentFields& a_fields, std::uint32_t a_slot)
		{
			auto& recordings = a_state.declaredRecordings[a_declared];
			const auto hash = RenderGraph::Frame::FieldsHash(a_fields);
			std::shared_ptr<const org::GraphHost::EpochRecording> recording;
			{
				const std::lock_guard lock(recordings.mutex);
				for (const auto& entry : recordings.entries)
					if (entry->hash == hash && entry->slot == a_slot && entry->fields == a_fields && entry->done) {
						recording = entry->recording;
						break;
					}
			}
			if (!recording || a_state.host->EpochRecordingVersionsCurrent(*recording, a_slot))
				return false;
			++recordings.versionGeneration;
			return true;
		}

		/**
		 * @brief Waits for a requested recording on the render owner, at most two seconds. A description asked for at the frame's start
		 * is normally recorded by the tap; a wait is counted. Null when it failed, was dropped or did not finish.
		 */
		std::shared_ptr<const org::GraphHost::EpochRecording> AwaitDeclared(State& a_state, std::uint32_t a_declared, const std::shared_ptr<SegmentRecording>& a_entry)
		{
			auto& recordings = a_state.declaredRecordings[a_declared];
			std::unique_lock lock(recordings.mutex);
			if (!a_entry->done) {
				ZoneScopedN("CS.ORG.Segment.WaitRecording");
				const auto start = std::chrono::steady_clock::now();
				recordings.ready.wait_for(lock, std::chrono::seconds(2), [&] { return a_entry->done; });
				Driver::NoteRecordingWait(static_cast<SegmentId>(a_declared),
					std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
				if (!a_entry->done)
					a_entry->error = "its recording did not finish in two seconds";
			}
			return a_entry->recording;
		}

		/**
		 * @brief A declared segment's tap: its end anchor's hook. The clients' commits write this frame slot's latch regions and gates,
		 * and the segment's one ticket is submitted with the recording of this frame's description of it. A tap the description does not
		 * cover is a tap error and draws nothing. Render owner, under the anchors' mutex.
		 */
		RunResult RunDeclared(State& a_state, const Point& a_point, std::uint32_t a_declared, const std::vector<RunClient>& a_clients)
		{
			const auto segment = static_cast<SegmentId>(a_declared);
			const char* tap = kInfo[IndexOf(a_point.anchor)].name;
			const auto& frame = Driver::Current();
			SegmentFields fields = frame.Segment(segment);
			if (!fields.declared) {
				Driver::NoteTapError(Driver::TapError::NotDeclared, segment, fmt::format("the {} tap fired", tap));
				return { RunStatus::Idle };
			}
			if (!fields.graph) {
				// Its graph is not built yet: its clients wait to join, as an anchor's do before its first build.
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::info("[ORG] frame segments: the {} tap fired before the segment's graph was built; it draws nothing until the build (logged once)", tap);
				return { RunStatus::Idle };
			}
			if (!Driver::NoteTap(segment, tap))
				return { RunStatus::Idle };
			auto& host = *a_state.host;
			auto& runtime = RenderGraphRuntime::Get();
			const std::uint32_t epoch = EpochOfDeclared(a_declared);
			std::shared_ptr<const org::GraphHost::EpochRecording> recording;
			std::string missing;
			// A build attached since the frame began (or since the recording was asked for) makes the recording of another graph: the
			// fields are read again and recorded for, once.
			for (int attempt = 0; attempt < 2 && !recording; ++attempt) {
				std::string why;
				if (fields.graph != host.BuildGeneration(epoch)) {
					fields = DescribeDeclared(a_state, a_declared, true);
					why = " (the segment's graph was built again since the frame began)";
				} else if (StaleVersions(a_state, a_declared, fields, frame.slot)) {
					// A slot of the segment's graph published a new version since the frame began: a recording binds the versions it
					// was made with.
					fields = DescribeDeclared(a_state, a_declared, true);
					why = " (a slot of the segment's graph published a new version since the frame began)";
				}
				auto entry = RequestDeclared(a_state, a_declared, fields, frame.slot, frame.frame, why);
				recording = AwaitDeclared(a_state, a_declared, entry);
				if (recording && !host.EpochRecordingCurrent(*recording)) {
					missing = "its recording is of an older build of the segment's graph";
					recording.reset();
					fields.graph = 0;
				} else if (recording && !host.EpochRecordingVersionsCurrent(*recording, frame.slot)) {
					// Published while it was recorded: once more, with the generation moved.
					missing = "a slot of the segment's graph published a new version while it was recorded";
					++a_state.declaredRecordings[a_declared].versionGeneration;
					recording.reset();
					fields.graph = 0;
				} else if (!recording) {
					missing = entry->error.empty() ? std::string("no recording") : entry->error;
				}
			}
			if (!recording) {
				Driver::NoteTapError(Driver::TapError::NoRecording, segment, missing);
				return { RunStatus::Idle };
			}
			const auto built = a_state.declaredSnapshots[a_declared].load(std::memory_order_acquire);
			const auto members = kDeclaredInfo[a_declared].members;
			const auto clients = SegmentClients(*built, a_declared);
			const bool sharedConstants = UsesSharedConstantsIn(built->clients, members);
			const auto expected = host.EpochCopyRun(epoch, frame.slot);
			// The clients that commit: those in the built graph and on in the description, with work this frame. An active client the
			// description turned off is a tap error, and its commit is not called.
			std::vector<std::shared_ptr<const ClientDesc>> committed;
			for (std::size_t i = 0; i < clients.size(); ++i) {
				const auto found = std::ranges::find_if(a_clients, [&](const RunClient& a_client) { return a_client.desc == clients[i]; });
				const bool active = found != a_clients.end() && found->active;
				const bool on = i < 32 && (fields.clientsOn >> i & 1u);
				if (!on) {
					if (active)
						Driver::NoteTapError(Driver::TapError::NotInDescription, segment, fmt::format("{} had work at the {} tap", clients[i]->id, tap));
					continue;
				}
				if (active)
					committed.push_back(clients[i]);
			}
			host.SetEpochCopy(epoch, frame.slot);
			RenderGraphRuntime::EpochResult result = RenderGraphRuntime::EpochResult::Stopped;
			NoteSequenceRun(a_point, false, a_clients);
			{
				RenderGraphRuntime::EpochBodyScope body(kDeclaredInfo[a_declared].segment);
				result = runtime.TryExecuteEpoch(kDeclaredInfo[a_declared].segment, [&](org::RenderGraph&) {
					const auto run = host.CurrentRun();
					if (!(run == expected))
						Driver::NoteTapError(Driver::TapError::SlotMismatch, segment,
							fmt::format("the run is lease {} copy {}, the slot's copy is lease {} copy {}", run.lease, run.copy, expected.lease, expected.copy));
					// The slot's latch regions still hold what its last run wrote: a block nobody writes this run reads zeros, as a new
					// lease's would.
					org::LatchBlock::ZeroStableRun(run.lease);
					RenderGraph::RunShapes::BeginRun();
					if (sharedConstants)
						SharedConstants::Commit();
					// Every gate closed first, those of registrations the graph still holds after they left too (RetiredGates).
					for (const auto& client : built->clients)
						if (AnchorMask(*client) & members)
							for (const auto& gate : client->gates)
								if (gate)
									gate->Close();
					for (const auto& retired : a_state.retired)
						if ((retired.mask & members) && built->serial < retired.serial)
							for (const auto& gate : retired.gates)
								gate->Close();
					for (const auto& client : committed)
						if (client->commit)
							client->commit(a_point);
					host.UseEpochRecording(recording);
				});
			}
			if (result == RenderGraphRuntime::EpochResult::Skipped)
				return { RunStatus::Refused };
			if (result == RenderGraphRuntime::EpochResult::EpochFailed)
				return { RunStatus::Failed };
			if (result != RenderGraphRuntime::EpochResult::Submitted) {
				StopAll(a_state, kDeclaredInfo[a_declared].names.label);
				return { RunStatus::Failed };
			}
			const std::uint32_t index = IndexOf(a_point.anchor);
			auto& runs = RunsSinceSeal(a_state, index);
			++runs;
			if (runs > a_state.peakRuns[index].load(std::memory_order_relaxed))
				a_state.peakRuns[index].store(runs, std::memory_order_relaxed);
			if (RenderGraph::RunShapes::On()) {
				std::uint64_t active = 0;
				for (const auto& client : committed)
					active += reinterpret_cast<std::uintptr_t>(client.get()) * 0x9E3779B97F4A7C15ull;
				RenderGraph::RunShapes::EndRun(index, kInfo[index].name, a_point.input, a_point.output, active);
			}
			if (kInfo[index].late && a_state.lateHook)
				a_state.lateHook();
			if (sharedConstants)
				SharedConstants::CheckAfterRun();
			PruneRetired(a_state);
			// One-shot passes in the graph this run submitted have run: the next build leaves them out, and the next frame's description
			// names that build.
			bool ranOnce = false;
			for (auto& client : a_state.clients)
				for (std::size_t i = 0; i < client.desc->passes.size(); ++i)
					if (client.armed[i] && client.armed[i] <= built->serial && (members >> IndexOf(client.desc->passes[i].anchor) & 1u)) {
						client.armed[i] = 0;
						ranOnce = true;
					}
			if (ranOnce) {
				Publish(a_state);
				Rebuild(a_state, members, "one-shot passes ran");
			}
			// Last: an executed callback may register, refresh or rearm.
			for (const auto& client : committed)
				if (client->executed)
					client->executed(a_point);
			return { RunStatus::Ran };
		}
	}

	bool SegmentsOn()
	{
		static const bool on = [] {
			if (!RenderGraph::Engine::Native::Requested())
				return false;
			// Native mode, default on (=0 off, =2 on with the segments' executions open).
			if (Switches::Number("CS_ORG_SEGMENTS", 1) == 0) {
				logger::info("[ORG] frame segments off (CS_ORG_SEGMENTS=0): every anchor keeps its own epoch");
				return false;
			}
			if (RegionsOn()) {
				logger::warn("[ORG] frame segments: off, the frame phases are on (CS_ORG_PHASES=1)");
				return false;
			}
			if (!RenderGraphRuntime::EpochsEnabled() || Switches::Equals("CS_ORG_ASYNC_EPOCHS", "0") || Switches::Equals("CS_ORG_REUSE_RECORDINGS", "0")) {
				logger::warn("[ORG] frame segments need epochs, async epochs and replayable recordings; every anchor keeps its own epoch");
				return false;
			}
			logger::info("[ORG] frame segments on (CS_ORG_SEGMENTS=0 turns them off): the pre-depth segment's anchors run in its epoch, recorded per description");
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
		// The segments a world frame draws: every one in stage 1. A frozen world's capture frame draws the world once too.
		const bool world = a_frame.kind == Kind::World || (a_frame.kind == Kind::FrozenWorld && a_frame.capture);
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d])
				continue;
			auto& fields = a_frame.segments[d];
			fields = DescribeDeclared(state, d, world);
			// A version the kept recording binds was replaced since: a new description now, at the frame's start.
			if (fields.declared && fields.graph && StaleVersions(state, d, fields, a_frame.slot))
				fields = DescribeDeclared(state, d, world);
			// Asked for now, so the host's thread records a new description while the engine draws the shadow maps: this slot's copy
			// first, then the next slots', which the next frames take. Each copy is recorded once its last run is done, so only
			// this frame's tap may wait for a recording.
			if (fields.declared && fields.graph)
				for (std::uint32_t ahead = 0; ahead < RenderGraphRuntime::kGameFramesInFlight; ++ahead)
					(void)RequestDeclared(state, d, fields, (a_frame.slot + ahead) % RenderGraphRuntime::kGameFramesInFlight, a_frame.frame, "");
		}
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
		// Declared segments: this hook is a segment's tap. The segment runs whenever the frame's description declares it.
		if (const auto declared = DeclaredOfAnchor(state, index); declared < kDeclared)
			return RunDeclared(state, a_point, declared, clients);
		if (!work)
			return { RunStatus::Idle };
		// Phases: a merged anchor whose passes are in its phase's built graph writes into the phase's held run.
		if (RegionsOn()) {
			const Phase phase = CurrentPhase(state);
			const auto p = PhaseIndex(phase);
			const auto snapshot = state.snapshot.load(std::memory_order_acquire);
			if (state.phaseEpochs[p] && snapshot && (snapshot->merged >> index & 1u)) {
				const auto phaseBuilt = state.phaseSnapshots[p].load(std::memory_order_acquire);
				const bool member = phaseBuilt && (phaseBuilt->members[p] >> index & 1u);
				// Every active client is in the phase's graph: one registered since it was built runs in the anchor's own graph until then.
				const bool current = member && std::ranges::all_of(clients, [&](const RunClient& a_client) { return !a_client.active || a_client.serial <= phaseBuilt->serial; });
				if (current)
					return Capture(state, a_point, phase, phaseBuilt, clients);
				if (!(state.learned[p] >> index & 1u))
					LearnMember(state, phase, index);
			}
		}
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
		// A region's held execution goes first: its runs came before this one (ExecuteEpoch would submit it too; here it is counted as this).
		FlushRegions(Interned(fmt::format("an anchor with its own epoch ({})", kInfo[index].name)), static_cast<std::uint32_t>(segment));
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
		// Last: an executed callback may register, refresh or rearm.
		for (const auto& client : clients)
			if (client.active && client.desc->executed && client.serial <= built)
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
		return true;
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
		/** @brief The phases' report since the last one: executions, the runs they held, and why each was submitted. */
		void LogRegionStats()
		{
			auto lock = Guard();
			auto& state = Anchors();
			if (!RegionsOn() || std::ranges::none_of(state.phaseEpochs, [](bool a_epoch) { return a_epoch; }))
				return;
			auto& stats = state.phaseStats;
			std::string causes;
			for (const auto& [why, count] : stats.causes)
				causes += fmt::format("{}{} {}", causes.empty() ? "" : ", ", why, count);
			std::string members;
			if (const auto snapshot = state.snapshot.load(std::memory_order_acquire))
				for (std::uint32_t p = 0; p < kPhases; ++p) {
					std::string names;
					for (std::uint32_t i = 0; i < kAnchors; ++i)
						if (snapshot->members[p] >> i & 1u)
							names += fmt::format("{}{}", names.empty() ? "" : " ", kInfo[i].name);
					members += fmt::format("{}{} [{}]", members.empty() ? "" : "; ", kPhaseNames[p].label, names);
				}
			// Each anchor that keeps its own epoch, with the passes that keep it there (not gated, or one-shot).
			std::string ungated;
			if (const auto snapshot = state.snapshot.load(std::memory_order_acquire))
				for (std::uint32_t i = 0; i < kAnchors; ++i) {
					if (!state.reserved[i] || (snapshot->merged >> i & 1u))
						continue;
					std::string passes;
					for (const auto& scheduled : snapshot->passes)
						if (IndexOf(scheduled.pass->anchor) == i && (!scheduled.pass->gated || scheduled.pass->once))
							passes += fmt::format("{}{}{}", passes.empty() ? "" : " ", scheduled.pass->name, scheduled.pass->once ? "(once)" : "");
					if (!passes.empty())
						ungated += fmt::format("{}{} [{}]", ungated.empty() ? "" : "; ", kInfo[i].name, passes);
				}
			// Joins: runs that went into an execution another run had opened (runs minus executions).
			logger::info(
				"[ORG] frame phases: {} executions held {} anchor runs ({:.2f} each, {} joins); {} lost to a build, {} failed; submitted before: {}; "
				"members: {}; anchors with ungated passes (their own epochs): {}",
				stats.executions, stats.runs, stats.executions ? double(stats.runs) / double(stats.executions) : 0.0, stats.runs - stats.executions,
				stats.lost, stats.failed, causes.empty() ? std::string("nothing") : causes, members, ungated.empty() ? std::string("none") : ungated);
			stats = {};
		}
	}

	void LogRunStats()
	{
		const auto& state = Anchors();
		std::string runs;
		for (std::uint32_t i = 0; i < kAnchors; ++i)
			if (state.reserved[i] && state.frozen)
				runs += fmt::format("{}{} {}", runs.empty() ? "" : ", ", kInfo[i].name, state.peakRuns[i].load(std::memory_order_relaxed));
		if (!runs.empty())
			logger::info("[ORG] frame anchors, peak runs in one frame: {}", runs);
		LogRegionStats();
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
			DropFailedClients(kDeclaredInfo[declared].members, kDeclaredInfo[declared].names.label, failed, a_error);
			return;
		}
		if (anchor == Anchor::Count) {
			// A phase's epoch: the clients of its members in the failed build are dropped, as for an anchor's.
			const Phase phase = PhaseOfEpoch(a_epoch);
			if (phase == Phase::None)
				return;
			const auto r = PhaseIndex(phase);
			const auto failed = state.phaseSnapshots[r].load(std::memory_order_acquire);
			if (a_keptGraph)
				state.phaseSnapshots[r].store(state.phasePrevious[r].load(std::memory_order_acquire), std::memory_order_release);
			if (state.held && state.held->phase == phase)
				DropHeld(state);
			DropFailedClients(failed ? failed->members[r] : 0u, kPhaseNames[r].label, failed, a_error);
			return;
		}
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
		// Declared segments: an epoch for each whose anchors were reserved; those anchors have none of their own.
		if (SegmentsOn())
			for (std::uint32_t d = 0; d < kDeclared; ++d) {
				std::uint32_t members = 0;
				for (std::uint32_t i = 0; i < kAnchors; ++i)
					if (state.reserved[i] && (kDeclaredInfo[d].members >> i & 1u))
						members |= 1u << i;
				if (!members)
					continue;
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
		// Phases: an epoch for each, at its place in the frame, with the census's members to start with.
		if (!RegionsOn())
			return;
		for (std::uint32_t r = 0; r < kPhases; ++r) {
			state.phaseEpochs[r] = true;
			state.learned[r] = PhaseSeed(static_cast<Phase>(r));
			const auto segment = static_cast<std::uint32_t>(PhaseSegment(static_cast<Phase>(r)));
			const std::uint32_t epoch = RenderGraphRuntime::EpochOf(static_cast<Segment>(segment));
			if (std::ranges::find(a_epochs, epoch) != a_epochs.end())
				continue;
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
		std::string regions;
		for (std::uint32_t r = 0; r < kPhases; ++r)
			if (state.phaseEpochs[r]) {
				a_host.SetEpochTicketsAhead(EpochOfPhase(static_cast<Phase>(r)), most);
				regions += fmt::format("{}{} (epoch {})", regions.empty() ? "" : ", ", kPhaseNames[r].label, EpochOfPhase(static_cast<Phase>(r)));
			}
		if (!regions.empty())
			logger::info(
				"[ORG] frame phases: {}; an anchor whose passes are all gated runs in the epoch of the phase it runs in, and a phase's "
				"runs share one execution",
				regions);
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
	bool RegionsOn() { return false; }
	bool SegmentsOn() { return false; }
	bool IsMainThread() { return true; }
	void BeginFrame(RenderGraph::Frame::Description&) {}
	std::vector<std::uint32_t> SegmentEpochs() { return {}; }
	void FlushRegions(const char*, std::uint32_t) {}
	void NoteEpochSubmitted(std::uint32_t) {}
	void LatchMainThread() {}
	bool IsRenderOwner() { return true; }
	void ClaimRenderOwnership() {}
	bool RequestJoin(std::string_view, Anchor) { return false; }
	bool CanRun(std::string_view, Anchor) { return false; }
	std::uint32_t PeakRunsPerFrame(Anchor) { return 0; }
	bool BuildPending(std::string_view) { return false; }
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

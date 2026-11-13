#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/FrameAnchors.h"

#	include "Globals.h"
#	include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#	include "RenderGraph/Engine/Native/Native.h"
#	include "RenderGraph/Engine/Targets/EngineTargets.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/BuildManager.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/RunShapes.h"
#	include "RenderGraph/SharedConstants.h"
#	include "RenderGraph/Switches.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>

#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <Resources/TextureDescription.h>
#	include <Resources/VersionedTexture.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <map>
#	include <mutex>

namespace FrameAnchors
{
	namespace
	{
		using Segment = RenderGraphRuntime::Segment;

		constexpr std::uint32_t kAnchors = static_cast<std::uint32_t>(Anchor::Count);
		static_assert(static_cast<std::uint32_t>(Segment::AnchorMovieTiles) - static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass) + 1 == kAnchors,
			"one segment per anchor, in the anchors' order");
		static_assert(static_cast<std::uint32_t>(Segment::Count) <= 64, "TakeSubmittedSegments holds one bit per segment");

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
		} };

		// Where each segment runs in a frame: AddToFrame puts an anchor before the first epoch of the order that runs after it. A
		// segment's place only has to be right against the anchors next to it; the order names the frame,
		// while the ledgers order the work whatever runs.
		constexpr std::array<Segment, static_cast<std::size_t>(Segment::Count)> kTimeline{
			Segment::AnchorDecalQueries,       // the decal occlusion tests, at the frame's start before Main::Draw
			Segment::AnchorEngineShadowClear,  // each shadow map slice's clear, before its draws (nominally here)
			Segment::ShadowView,
			Segment::SkyOcclusion,
			Segment::AnchorEarlyPrepass,      // Main::RenderShadowMaps returned
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
			Segment::AnchorPrepass,             // after every feature's Prepass
			Segment::AnchorSky,                 // the main pass's sky batch, before Drawcall Limit Fix's colour epoch
			Segment::MainOpaque,
			Segment::AnchorDeferredPasses,     // EndDeferred, after the blended decals
			Segment::AnchorDeferredComposite,  // DeferredPasses, after the D3D11 SSGI and SSS
			Segment::AnchorPostComposite,      // DeferredPasses' end, after the composite
			Segment::AnchorEngineDraws,        // the water, before the forward epoch (nominally here: the shadow masks and the water mask run before, the blood after)
			Segment::Forward,
			Segment::AnchorEngineQueries,  // the sun's occlusion queries, after Main::RenderWorld's accumulator (nominally here: a second runs later in the frame)
			Segment::FirstPerson,
			Segment::AnchorVolumetricLighting,  // RenderWorldPostProcess's VL apply and blur, before the composites and Main_PostProcessing
			Segment::AnchorPostProcessing,   // Main_PostProcessing, before the upscaler
			Segment::AnchorUpscale,          // after the upscaler's evaluate
			Segment::AnchorImageSpace,       // the ImageSpace chain's batches (pre-tonemap composites, the HDR effect), a few times a frame
			Segment::AnchorTonemap,          // in the ImageSpace chain after it
			Segment::AnchorFrameGeneration,  // after the ImageSpace chain, before frame generation
			Segment::AnchorMovieTiles,       // a Bink movie inside another frame, at its tile draw (nominally here: the frame's UI time)
			Segment::AnchorInterface,        // Scaleform's displays (UI::DrawInterface), several times a frame
			Segment::AnchorPresent,
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
			std::shared_ptr<org::VersionedTexture> slot;  // an id import's
			std::weak_ptr<org::VersionedTexture> looseSlot;  // a loose one's
			std::string id;  // the id (or the loose holder's name) it was made under, for the logs
			std::uint64_t image = 0;  // the current version's VkImage (ID3D12Resource on D3D12)
			DxvkOrgInteropImageInfo info{};  // the current version's image, to import it again with more views
			org::TextureDescription views;  // the views the current version has: what a new importer's are compared with

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

		/** @brief The anchors' state. Render thread, but the snapshot and the built serials, which a build writes and reads. */
		struct State
		{
			std::array<bool, kAnchors> reserved{};
			bool frozen = false;  // AddToFrame ran: the reservations are the frame's epochs
			org::GraphHost* host = nullptr;
			std::vector<Client> clients;
			std::map<std::string, std::shared_ptr<EngineImport>, std::less<>> imports;  // by graph id; the ids of one image share one
			std::vector<std::weak_ptr<EngineImport>> loose;  // ShareImage's slots no id names
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
			for (std::uint32_t i = 0; i < kAnchors; ++i)
				if ((a_anchorMask >> i & 1u) && a_state.reserved[i])
					RenderGraph::Builds::Request(EpochOfAnchor(static_cast<Anchor>(i)), reason);
		}

		/**
		 * @brief Rebuild for a resource identity change no version stands for (a replaced import, a resized twin that is not
		 * versioned): the anchors' epochs, through GraphResources::RequestStructuralRebuild.
		 */
		void StructuralRebuild(State& a_state, std::uint32_t a_anchorMask, std::string_view a_reason)
		{
			if (!a_state.host)
				return;
			std::vector<std::uint32_t> epochs;
			for (std::uint32_t i = 0; i < kAnchors; ++i)
				if ((a_anchorMask >> i & 1u) && a_state.reserved[i])
					epochs.push_back(EpochOfAnchor(static_cast<Anchor>(i)));
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
			std::erase_if(a_state.retired, [&](const RetiredGates& a_retired) {
				for (std::uint32_t i = 0; i < kAnchors; ++i)
					if ((a_retired.mask >> i & 1u) && a_state.reserved[i] && a_state.built[i].load(std::memory_order_acquire) < a_retired.serial)
						return false;
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
				if ((mask >> i & 1u) && a_state.reserved[i] && a_client.serial > a_state.built[i].load(std::memory_order_acquire))
					return true;
			return false;
		}

		/**
		 * @brief The clients with passes at the anchor in a failed build (a_failed: the snapshot it was built from), still registered as
		 * they were then, are dropped and return to D3D11; the anchor, and their other anchors, are built again without them. A client
		 * registered again since is not in that build, and its rebuild is already asked for. Render thread.
		 */
		void DropFailedClients(std::uint32_t a_index, const std::shared_ptr<const Snapshot>& a_failed, const std::string& a_error)
		{
			auto lock = Guard();
			auto& state = Anchors();
			const auto anchor = static_cast<Anchor>(a_index);
			std::vector<std::shared_ptr<const ClientDesc>> dropped;
			std::uint32_t mask = 1u << a_index;
			if (a_failed)
				for (const auto& client : a_failed->clients) {
					if (!HasPasses(*client, anchor))
						continue;
					const auto found = std::ranges::find_if(state.clients, [&](const Client& a_client) { return a_client.desc == client; });
					if (found == state.clients.end())
						continue;
					mask |= AnchorMask(*client);
					dropped.push_back(client);
					state.clients.erase(found);
				}
			if (dropped.empty()) {
				logger::error("[ORG] frame anchors: the {} epoch failed and none of its build's clients is still registered; it runs nothing new until a "
							  "client registers or changes. Why: {}",
					Name(anchor), a_error);
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
			logger::error("[ORG] frame anchors: the {} epoch failed; its clients {} are dropped and return to D3D11, and the anchor is built again "
						  "without them. Why: {}",
				Name(anchor), ids, a_error);
			for (const auto& client : dropped)
				if (client->onFailed)
					client->onFailed();
		}

		/**
		 * @brief The anchors' passes in each graph: an anchor epoch's graph takes its anchor's passes and registers the clients'
		 * resources and the engine imports; every other epoch's graph gets nothing from it.
		 */
		class AnchorExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			explicit AnchorExtension(std::shared_ptr<const Snapshot> a_snapshot) :
				snapshot(std::move(a_snapshot)) {}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				if (!snapshot || !IsAnchorGraph(a_graph))
					return;
				// What the run that submits this graph reads (ClientDesc::executed, one-shot passes): the graph holds this snapshot's passes.
				// Recorded when the graph is installed: at its attach for a build the build manager makes (on its own thread, and possibly
				// dropped), at once for one the host makes on the owner thread. A failed managed build drops its clients (DropFailedClients).
				const auto index = IndexOf(AnchorOfEpoch(a_graph.PassEpochFilter()));
				const auto commit = [index, built = snapshot] {
					auto& anchors = Anchors();
					anchors.built[index].store(built->serial, std::memory_order_release);
					anchors.previousSnapshots[index].store(anchors.builtSnapshots[index].load(std::memory_order_acquire), std::memory_order_release);
					anchors.builtSnapshots[index].store(built, std::memory_order_release);
				};
				if (!RenderGraph::Builds::Defer(commit, [index, failed = snapshot](const std::string& a_error) { DropFailedClients(index, failed, a_error); }))
					commit();
				// Each import's slot is the id's resolver, so a declaration by id follows its versions too.
				for (const auto& [id, slot] : snapshot->imports)
					if (const org::ResourceIdentifier key(id); !a_graph._resolverMap.contains(key))
						a_graph.RegisterResolver(key, slot);
				for (const auto& client : snapshot->clients)
					if (client->registerResources)
						client->registerResources(a_graph);
				if (UsesSharedConstants(snapshot->clients, AnchorOfEpoch(a_graph.PassEpochFilter())))
					SharedConstants::Register(a_graph);
			}

			void GatherStructuralPasses(org::RenderGraph& a_graph, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				if (!snapshot || !IsAnchorGraph(a_graph))
					return;
				const std::uint32_t epoch = a_graph.PassEpochFilter();
				// The shared constants' upload first: the passes that read them follow it (the graph orders them by the buffers).
				if (UsesSharedConstants(snapshot->clients, AnchorOfEpoch(epoch)))
					if (auto upload = SharedConstants::MakeUploadPass())
						a_out.push_back(std::move(org::RenderGraph::ExternalPassDesc::Copy("cs.shared.upload", std::move(upload))
								.PreferQueue(org::QueueKind::Graphics)
								.Epoch(epoch)));
				for (const auto& scheduled : snapshot->passes) {
					const auto& pass = *scheduled.pass;
					if (EpochOfAnchor(pass.anchor) != epoch)
						continue;
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
					// Recorded in DXVK's stream after the run's commit, on the graphics queue only (PassDesc::atExecution).
					if (pass.atExecution && pass.queue == PassQueue::Default && a_graph.PassEpochInHostStream())
						desc.RecordAtExecution();
					a_out.push_back(std::move(desc.Epoch(epoch)));
				}
			}

		private:
			static bool IsAnchorGraph(const org::RenderGraph& a_graph)
			{
				return AnchorOfEpoch(a_graph.PassEpochFilter()) != Anchor::Count;
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
		 * image's graph-owned texture (Engine::Native::GraphTextureOf; a_texture may be null, then it is looked up by image) when it has
		 * every view asked for. Otherwise the image imported, with the graph-owned texture's views added when there is one, so a later
		 * importer that asks for those finds them. The one place an image becomes a graph resource for the anchors and DCLF. Null,
		 * logged, on failure.
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
					if (Covers(made, a_desc)) {
						a_desc = made;
						DescribeSubresources(a_desc, a_image);
						// It may have been set aside for an import of its image before: it is the image's graph resource again.
						RenderGraph::NoteImageResource(image, owned, a_id);
						return owned;
					}
					std::string conflict;
					a_desc = MergeViews(made, a_desc, conflict);
					static std::atomic<std::uint32_t> logged{ 0 };
					if (LogImportLine(logged))
						logger::warn("[ORG] frame anchors: {}: the graph-owned texture lacks a view asked for{}{}; its image is imported with both sets of views instead",
							a_id, conflict.empty() ? "" : ": ", conflict);
					// Never handed to a graph while this import stands for its image.
					RenderGraph::RetireImageResource(owned.get());
				}
			}
			DescribeSubresources(a_desc, a_image);
			auto resource = RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), a_image, a_desc, a_id.c_str());
			if (!resource)
				logger::warn("[ORG] frame anchors: {} could not be imported", a_id);
			return resource;
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
			DescribeSubresources(merged, a_import->info);
			auto& runtime = RenderGraphRuntime::Get();
			const auto old = slot->CurrentResource();
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
		const std::uint32_t first = static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass);
		if (a_segment < first || a_segment - first >= kAnchors)
			return nullptr;
		return &kInfo[a_segment - first].names;
	}

	bool IsUnusedSegment(std::uint32_t a_segment)
	{
		const std::uint32_t first = static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass);
		if (a_segment < first || a_segment - first >= kAnchors)
			return false;
		const auto& state = Anchors();
		return !state.frozen || !state.reserved[a_segment - first];
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
		return client && index < kAnchors && client->serial <= state.built[index].load(std::memory_order_acquire);
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
				a_state.resizeGeneration = generation;
				PublishedResources::Refresh();
				Publish(a_state);
				for (auto& client : a_state.clients)
					MarkCurrent(a_state, client);
				StructuralRebuild(a_state, ~0u, "graph resources resized (a twin that is not versioned, or another shape class)");
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

	RunResult Run(const Point& a_point)
	{
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
			logger::error("[ORG] frame anchors: the {} epoch failed; {} clients return to D3D11", Name(a_point.anchor), state.clients.size());
			auto failed = std::move(state.clients);
			state.clients.clear();
			state.retired.clear();
			Publish(state);
			for (const auto& client : failed)
				if (client.desc->onFailed)
					client.desc->onFailed();
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
		if (client->serial <= state.built[index].load(std::memory_order_acquire))
			return true;
		// Register or Refresh asked for this build already. Asked again only when no request is pending: a request each frame while
		// the build runs off the render thread would supersede it at every attach. The build manager builds it; its attach records it.
		if (state.host && !state.host->RebuildRequested(EpochOfAnchor(a_anchor)))
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
		if (!client || !InGraphs(*client->desc) || client->serial > state.built[index].load(std::memory_order_acquire))
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

	void LogRunStats()
	{
		const auto& state = Anchors();
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
		DropFailedClients(index, failed, a_error);
	}

	void AddToFrame(std::vector<std::uint32_t>& a_epochs)
	{
		auto& state = Anchors();
		if (!RenderGraphRuntime::EpochsEnabled())
			return;
		state.frozen = true;
		for (std::uint32_t i = 0; i < kAnchors; ++i) {
			if (!state.reserved[i])
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
			if (state.reserved[i])
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
			if (state.reserved[i])
				reserved += fmt::format("{}{} (epoch {})", reserved.empty() ? "" : ", ", kInfo[i].name, EpochOfAnchor(static_cast<Anchor>(i)));
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
	bool Register(ClientDesc) { return false; }
	void Unregister(std::string_view) {}
	void Refresh(std::string_view) {}
	RunResult Run(const Point&) { return {}; }
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

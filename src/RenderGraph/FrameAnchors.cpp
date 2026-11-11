#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/FrameAnchors.h"

#	include "Globals.h"
#	include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#	include "RenderGraph/Engine/Targets/EngineTargets.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/SharedConstants.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>

#	include <Resources/ExternalTextureResource.h>
#	include <Resources/TextureDescription.h>
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
		static_assert(static_cast<std::uint32_t>(Segment::AnchorEngineQueries) - static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass) + 1 == kAnchors,
			"one segment per anchor, in the anchors' order");
		static_assert(static_cast<std::uint32_t>(Segment::Count) <= 64, "TakeSubmittedSegments holds one bit per segment");

		constexpr const char* kExtensionId = "cs.org.frame-anchors";

		struct AnchorInfo
		{
			const char* name;
			SegmentNames names;
			bool late;  // may be the last graph work before an upscaler, frame generation or the present
			// The most runs a frame the anchor's epoch is sized for (GraphHost::Desc::epochRuns): its ring of frame slots grows up to
			// kGameFramesInFlight times this instead of the host waiting for a run of the same frame, which DXVK has not submitted yet.
			// 1: an anchor that runs once a frame.
			std::uint32_t runsPerFrame = 1;
		};

		constexpr std::array<AnchorInfo, kAnchors> kInfo{ {
			// Runs a frame: one per cubemap the engine renders (BSCubeMapCamera::RenderCubemap)
			{ "ReflectionsPrepass", { "CS anchor: reflections prepass", "CS anchor: reflections prepass inputs", "FrameAnchors::ReflectionsPrepass / ", "cs.anchor.reflections-prepass" }, false, 2 },
			// Runs a frame: one per reflection face whose sky is drawn
			{ "SkyReflections", { "CS anchor: reflection sky", "CS anchor: reflection sky inputs", "FrameAnchors::SkyReflections / ", "cs.anchor.sky-reflections" }, false, 6 },
			{ "EarlyPrepass", { "CS anchor: early prepass", "CS anchor: early prepass inputs", "FrameAnchors::EarlyPrepass / ", "cs.anchor.early-prepass" }, false, 1 },
			{ "DepthDone", { "CS anchor: depth done", "CS anchor: depth done inputs", "FrameAnchors::DepthDone / ", "cs.anchor.depth-done" }, false, 1 },
			{ "Prepass", { "CS anchor: prepass", "CS anchor: prepass inputs", "FrameAnchors::Prepass / ", "cs.anchor.prepass" }, false, 1 },
			{ "Sky", { "CS anchor: sky", "CS anchor: sky inputs", "FrameAnchors::Sky / ", "cs.anchor.sky" }, false, 1 },
			{ "DeferredPasses", { "CS anchor: deferred passes", "CS anchor: deferred passes inputs", "FrameAnchors::DeferredPasses / ", "cs.anchor.deferred-passes" }, false, 1 },
			{ "DeferredComposite", { "CS anchor: deferred composite", "CS anchor: deferred composite inputs", "FrameAnchors::DeferredComposite / ", "cs.anchor.deferred-composite" }, false, 1 },
			{ "PostComposite", { "CS anchor: post-composite", "CS anchor: post-composite inputs", "FrameAnchors::PostComposite / ", "cs.anchor.post-composite" }, false, 1 },
			{ "PostProcessing", { "CS anchor: post-processing", "CS anchor: post-processing inputs", "FrameAnchors::PostProcessing / ", "cs.anchor.post-processing" }, true, 1 },
			{ "Upscale", { "CS anchor: upscale", "CS anchor: upscale inputs", "FrameAnchors::Upscale / ", "cs.anchor.upscale" }, true, 1 },
			{ "Tonemap", { "CS anchor: tonemap", "CS anchor: tonemap inputs", "FrameAnchors::Tonemap / ", "cs.anchor.tonemap" }, true, 1 },
			{ "FrameGeneration", { "CS anchor: frame generation", "CS anchor: frame generation inputs", "FrameAnchors::FrameGeneration / ", "cs.anchor.frame-generation" }, true, 1 },
			{ "Present", { "CS anchor: present", "CS anchor: present inputs", "FrameAnchors::Present / ", "cs.anchor.present" }, true, 1 },
			{ "Overlay", { "CS anchor: overlay", "CS anchor: overlay inputs", "FrameAnchors::Overlay / ", "cs.anchor.overlay" }, true, 1 },
			{ "HDROutput", { "CS anchor: HDR output", "CS anchor: HDR output inputs", "FrameAnchors::HDROutput / ", "cs.anchor.hdr-output" }, true, 1 },
			{ "Capture", { "CS anchor: capture", "CS anchor: capture inputs", "FrameAnchors::Capture / ", "cs.anchor.capture" }, true, 1 },
			// Runs a frame: one per shadow map slice cleared
			{ "EngineShadowClear", { "CS anchor: engine shadow clear", "CS anchor: engine shadow clear inputs", "FrameAnchors::EngineShadowClear / ", "cs.anchor.engine-shadow-clear" }, false, 8 },
			// Runs a frame: one per reflection face batch
			{ "EngineCubeClear", { "CS anchor: engine cube clear", "CS anchor: engine cube clear inputs", "FrameAnchors::EngineCubeClear / ", "cs.anchor.engine-cube-clear" }, false, 4 },
			// Runs a frame: one per batch of the other engine clears
			{ "EngineFrameClear", { "CS anchor: engine frame clear", "CS anchor: engine frame clear inputs", "FrameAnchors::EngineFrameClear / ", "cs.anchor.engine-frame-clear" }, false, 8 },
			// Runs a frame: Main::RenderDepth, FinishAccumulating and DeferredPasses
			{ "EngineDepthCopy", { "CS anchor: engine depth copy", "CS anchor: engine depth copy inputs", "FrameAnchors::EngineDepthCopy / ", "cs.anchor.engine-depth-copy" }, false, 3 },
			// Runs a frame: one per Scaleform segment drawn on the graph (a display, and one more per D3D11 part); 6 was not passed in a menu tour
			{ "Interface", { "CS anchor: interface", "CS anchor: interface inputs", "FrameAnchors::Interface / ", "cs.anchor.interface" }, true, 6 },
			// Runs a frame: one per batch of ImageSpace draws and dispatches, the sum of ImageSpace.cpp's kScopeRuns: RenderSSAO's, the
			// reflection pass's and the HDR effect's (two each: a batch cut short by a draw the graph does not take), the pre-tonemap
			// composites' (one), and one per end-of-frame effect call (depth of field or the distant blur, its two compute blurs,
			// refraction, the radial blur, the snow copy, the water blend, the LDR downsample: eight). Late: the TAA or the upscaler can follow a batch inside the chain.
			{ "ImageSpace", { "CS anchor: image space", "CS anchor: image space inputs", "FrameAnchors::ImageSpace / ", "cs.anchor.image-space" }, true, 15 },
			// Runs a frame: the world's shadow masks, the water mask, the water, a menu scene's shadow masks and the screen blood.
			{ "EngineDraws", { "CS anchor: engine draws", "CS anchor: engine draws inputs", "FrameAnchors::EngineDraws / ", "cs.anchor.engine-draws" }, true, 5 },
			// Runs a frame: one per sun occlusion query the engine submits (Sun::SubmitOcclusionQuery's indices 0 to 2; two seen a frame).
			{ "EngineQueries", { "CS anchor: engine queries", "CS anchor: engine queries inputs", "FrameAnchors::EngineQueries / ", "cs.anchor.engine-queries" }, false, 3 },
		} };

		// Where each segment runs in a frame: AddToFrame puts an anchor before the first epoch of the order that runs after it. A
		// segment's place only has to be right against the anchors next to it; the order sizes the frame slots and names the frame,
		// while the ledgers order the work whatever runs.
		constexpr std::array<Segment, static_cast<std::size_t>(Segment::Count)> kTimeline{
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
			Segment::AnchorPostProcessing,   // Main_PostProcessing, before the upscaler
			Segment::AnchorUpscale,          // after the upscaler's evaluate
			Segment::AnchorImageSpace,       // the ImageSpace chain's batches (pre-tonemap composites, the HDR effect), a few times a frame
			Segment::AnchorTonemap,          // in the ImageSpace chain after it
			Segment::AnchorFrameGeneration,  // after the ImageSpace chain, before frame generation
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

		/** @brief An engine target imported for the anchors' passes, kept while the engine's texture stays the same. */
		struct EngineImport
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			std::shared_ptr<org::ExternalTextureResource> resource;
			std::string id;
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
			std::vector<std::pair<std::string, std::shared_ptr<org::ExternalTextureResource>>> imports;
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
			std::vector<Anchor> reserveOrder;  // Reserve's order: FitSlotBudget drops the last ones first
			bool frozen = false;  // AddToFrame ran: the reservations are the frame's epochs
			org::GraphHost* host = nullptr;
			std::vector<Client> clients;
			std::map<std::string, EngineImport, std::less<>> imports;  // by graph id
			std::uint64_t serial = 0;                                  // bumped by every Publish
			std::uint64_t resizeGeneration = 0;                        // GraphResources' last seen
			std::function<void()> lateHook;
			std::vector<RetiredGates> retired;
			// Per anchor: its submitted runs since the host's seal runsSeal (the last Present), and the most it ever had in one frame.
			std::array<std::uint32_t, kAnchors> runsThisFrame{};
			std::array<std::uint64_t, kAnchors> runsSeal{};
			std::array<std::atomic<std::uint32_t>, kAnchors> peakRuns{};
			std::uint64_t buildPointsSeen = 0;  // RenderGraphRuntime::BuildPoints at the last frame end
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
			for (const auto& [key, import] : a_state.imports)
				snapshot->imports.emplace_back(import.id, import.resource);
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
					a_state.host->RequestRebuild(EpochOfAnchor(static_cast<Anchor>(i)), reason);
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

		/** @brief The anchor epoch's ring of frame slots: kGameFramesInFlight times its runs a frame (GraphHost::Desc::epochRuns). */
		std::uint32_t RingCapacity(std::uint32_t a_index)
		{
			return RenderGraphRuntime::kGameFramesInFlight * kInfo[a_index].runsPerFrame;
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
				const auto index = IndexOf(AnchorOfEpoch(a_graph.PassEpochFilter()));
				auto& anchors = Anchors();
				anchors.built[index].store(snapshot->serial, std::memory_order_release);
				anchors.previousSnapshots[index].store(anchors.builtSnapshots[index].load(std::memory_order_acquire), std::memory_order_release);
				anchors.builtSnapshots[index].store(snapshot, std::memory_order_release);
				for (const auto& [id, resource] : snapshot->imports)
					a_graph.RegisterResource(org::ResourceIdentifier(id), resource);
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

		std::shared_ptr<org::ExternalTextureResource> Import(ID3D11Texture2D* a_texture, const std::string& a_id,
			const std::function<org::TextureDescription(const D3D11_TEXTURE2D_DESC&)>& a_describe)
		{
			auto lock = Guard();
			auto& state = Anchors();
			auto& runtime = RenderGraphRuntime::Get();
			if (!a_texture || !runtime.IsActive())
				return nullptr;
			// An engine target ORG owns (CS_ORG_ENGINE_TARGETS): its published texture, which every graph registers. Importing the
			// image again would make a second resource whose uses the graph does not order with the owner's.
			if (auto owned = RenderGraph::Engine::Targets::Find(a_texture))
				return owned;
			// Anything else is imported as DXVK's own texture, never as a tripwire handle.
			a_texture = RenderGraph::Engine::Tripwire::Unwrap(a_texture);
			const auto previous = state.imports.find(a_id);
			if (previous != state.imports.end() && previous->second.texture.get() == a_texture)
				return previous->second.resource;
			const bool replacing = previous != state.imports.end();
			// The same image under another id is a second graph resource: the graph orders neither's uses after the other's writes.
			for (const auto& [id, other] : state.imports)
				if (id != a_id && other.texture.get() == a_texture)
					logger::warn("[ORG] frame anchors: {} imports the image {} already imports; their uses are not ordered with each other", a_id, id);
			DxvkOrgInteropResourceInfo info{};
			if (!runtime.DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE) {
				logger::warn("[ORG] frame anchors: {} cannot be described", a_id);
				return nullptr;
			}
			D3D11_TEXTURE2D_DESC textureDesc{};
			a_texture->GetDesc(&textureDesc);
			auto desc = a_describe(textureDesc);
			// One entry per mip: the graph's views and barriers then cover the whole image.
			const std::uint32_t mips = (std::max)(info.image.mipLevels, 1u);
			for (std::uint32_t mip = 0; mip < mips; ++mip)
				desc.imageDimensions.push_back({ (std::max)(info.image.extent.width >> mip, 1u), (std::max)(info.image.extent.height >> mip, 1u), 0, 0 });
			// A cube (the engine's cubemap targets): its SRV is a cube view, and arraySize counts cubes.
			const std::uint32_t layers = (std::max)(info.image.arrayLayers, 1u);
			desc.isCubemap = (textureDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) && layers % 6 == 0;
			const std::uint32_t slices = desc.isCubemap ? layers / 6 : layers;
			desc.isArray = slices > 1;
			desc.arraySize = slices;
			auto resource = RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), info.image, desc, a_id.c_str());
			if (!resource) {
				logger::warn("[ORG] frame anchors: {} could not be imported", a_id);
				return nullptr;
			}
			EngineImport import;
			import.texture.copy_from(a_texture);
			import.resource = resource;
			import.id = a_id;
			state.imports[a_id] = std::move(import);
			Publish(state);
			// The imports are registered with every anchor graph. A new one is built into the graphs that looked its id up (a client
			// registering with it builds its own anchors again); one replaced is dropped, so every graph holding it is built again.
			if (replacing)
				Rebuild(state, ~0u, fmt::format("import '{}' replaced", a_id));
			else if (state.host)
				state.host->RequestRebuildFor(org::ResourceIdentifier(a_id), fmt::format("frame anchors: import '{}' added", a_id));
			return resource;
		}
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
		state.reserveOrder.push_back(a_anchor);
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
				Rebuild(a_state, ~0u, "graph resources resized");
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
		// Past the ring: the host would throw EpochRingFullError (an epoch it sizes for several runs) or wait for a run of this frame,
		// which DXVK has not submitted (one it sizes for one). Skipped instead, and logged once per anchor.
		auto& runs = RunsSinceSeal(state, index);
		if (runs >= RingCapacity(index)) {
			static std::array<std::atomic_flag, kAnchors> logged;
			if (!logged[index].test_and_set())
				logger::warn(
					"[ORG] frame anchors: {} ran {} times since the last Present, all its ring holds ({} runs a frame x {} frames); "
					"later runs this frame are skipped and their callers keep the engine's path",
					Name(a_point.anchor), runs, kInfo[index].runsPerFrame, RenderGraphRuntime::kGameFramesInFlight);
			return { RunStatus::RingFull };
		}
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
		if (result == RenderGraphRuntime::EpochResult::RingFull) {
			// The host's own count of unsealed runs was reached first (ExecuteEpoch logged it): nothing ran, nothing is dropped.
			return { RunStatus::RingFull };
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
		// The run counts against the ring, and the peak is what RunsPerFrame should be sized from (LogRunStats).
		++runs;
		if (runs > state.peakRuns[index].load(std::memory_order_relaxed)) {
			state.peakRuns[index].store(runs, std::memory_order_relaxed);
			if (runs > kInfo[index].runsPerFrame)
				logger::info("[ORG] frame anchors: {} ran {} times in one frame, above the {} it is sized for (its ring holds {})", Name(a_point.anchor),
					runs, kInfo[index].runsPerFrame, RingCapacity(index));
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
		// Register or Refresh asked for this build already; asking again costs nothing and covers a build that failed meanwhile
		// (OnEpochFailed keeps the old graph). OnFrameEnd, or Drawcall Limit Fix's build point, builds it.
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
		return RunsSinceSeal(state, index) < RingCapacity(index);
	}

	std::uint32_t RunsPerFrame(Anchor a_anchor)
	{
		return IndexOf(a_anchor) < kAnchors ? kInfo[IndexOf(a_anchor)].runsPerFrame : 0;
	}

	std::uint32_t PeakRunsPerFrame(Anchor a_anchor)
	{
		return IndexOf(a_anchor) < kAnchors ? Anchors().peakRuns[IndexOf(a_anchor)].load(std::memory_order_relaxed) : 0;
	}

	void OnFrameEnd()
	{
		auto& state = Anchors();
		if (!state.host || !IsRenderOwner())
			return;
		auto lock = Guard();
		auto& runtime = RenderGraphRuntime::Get();
		if (!runtime.IsActive())
			return;
		// A frame that had a build point (Drawcall Limit Fix's, in a world frame) builds there: nothing to do here.
		const std::uint64_t points = runtime.BuildPoints();
		const bool hadBuildPoint = points != state.buildPointsSeen;
		state.buildPointsSeen = points;
		if (hadBuildPoint || std::ranges::none_of(state.clients, [&](const Client& a_client) { return WaitsToJoin(state, a_client); }))
			return;
		// Between frames, and only while a client waits: a menu, a loading screen or a movie builds its joining clients here.
		const bool built = runtime.BuildIfRequested();
		state.buildPointsSeen = runtime.BuildPoints();
		static std::uint32_t s_builds = 0;
		if (built && (++s_builds <= 16 || s_builds % 100 == 0))
			logger::info("[ORG] frame anchors: graphs built at the frame's end for clients joining them (build {})", s_builds);
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
		(void)runtime.BuildIfRequested();
		state.buildPointsSeen = runtime.BuildPoints();
		const auto left = waiting();
		if (left == 0)
			logger::info("[ORG] frame anchors: graphs built at {}; every registered client is in its anchors' graphs", a_caller);
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
				runs += fmt::format("{}{} {}/{}", runs.empty() ? "" : ", ", kInfo[i].name, state.peakRuns[i].load(std::memory_order_relaxed), kInfo[i].runsPerFrame);
		if (!runs.empty())
			logger::info("[ORG] frame anchors, peak runs in one frame / sized for: {}", runs);
	}

	void FitSlotBudget(const std::vector<std::uint32_t>& a_epochs, std::uint32_t a_framesInFlight, std::uint32_t a_maxSlots)
	{
		auto& state = Anchors();
		if (!RenderGraphRuntime::EpochsEnabled())
			return;
		// Every epoch of the order that is not a reserved anchor's runs once a frame.
		std::uint64_t others = 0;
		for (const auto epoch : a_epochs) {
			const Anchor anchor = AnchorOfEpoch(epoch);
			if (anchor == Anchor::Count || !state.reserved[IndexOf(anchor)])
				++others;
		}
		const std::uint64_t frames = (std::max)(a_framesInFlight, 1u);
		const auto total = [&] {
			std::uint64_t runs = others;
			for (std::uint32_t i = 0; i < kAnchors; ++i)
				if (state.reserved[i])
					runs += kInfo[i].runsPerFrame;
			return runs * frames;
		};
		// Over the budget, the anchors reserved last lose their reservation: loud, and never a smaller ring than kInfo says.
		for (auto it = state.reserveOrder.rbegin(); it != state.reserveOrder.rend() && total() > a_maxSlots; ++it) {
			const std::uint32_t index = IndexOf(*it);
			if (!state.reserved[index])
				continue;
			const std::uint64_t over = total();
			state.reserved[index] = false;
			logger::error(
				"[ORG] frame anchor {}: reservation dropped. The frame slots ({} frames in flight x every epoch's runs a frame) would be {}, "
				"more than the budget of {} (RenderGraphRuntime::kMaxFrameSlots); its clients stay on D3D11. Lower an anchor's runs a frame in FrameAnchors.cpp (kInfo; "
				"CS_ORG_EPOCH_STATS=1 logs the peaks) or merge anchors",
				kInfo[index].name, frames, over, a_maxSlots);
		}
		std::string anchors;
		for (std::uint32_t i = 0; i < kAnchors; ++i)
			if (state.reserved[i])
				anchors += fmt::format("{}{}:{}", anchors.empty() ? "" : ", ", kInfo[i].name, kInfo[i].runsPerFrame);
		logger::info("[ORG] frame slot budget: {} of {} ({} frames x ({} other epochs + anchors' runs a frame: {}))", total(), a_maxSlots, frames, others,
			anchors.empty() ? "none" : anchors);
	}

	std::shared_ptr<org::ExternalTextureResource> EngineTarget(std::uint32_t a_target)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || a_target >= static_cast<std::uint32_t>(RE::RENDER_TARGET::kTOTAL))
			return nullptr;
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

	std::shared_ptr<org::ExternalTextureResource> EngineDepth(std::uint32_t a_depthStencil)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || a_depthStencil >= static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL))
			return nullptr;
		const auto target = renderer->GetDepthStencilData().depthStencils[a_depthStencil];
		return Import(target.texture, fmt::format("cs.engine.depth.{}", a_depthStencil),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeDepth(target, a_desc); });
	}

	std::shared_ptr<org::ExternalTextureResource> EngineCubemap(std::uint32_t a_cubemap)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || a_cubemap >= static_cast<std::uint32_t>(RE::RENDER_TARGETS_CUBEMAP::kTOTAL))
			return nullptr;
		const auto target = renderer->GetRendererData().cubemapRenderTargets[a_cubemap];
		// The faces' RTVs share one format; the graph makes its own views of each face.
		return Import(target.texture, fmt::format("cs.engine.cubemap.{}", a_cubemap),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeColour(a_desc, target.SRV, target.cubeSideRTV[0], nullptr); });
	}

	std::shared_ptr<org::ExternalTextureResource> D3D11Texture(std::string_view a_name, ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_srv,
		ID3D11UnorderedAccessView* a_uav, ID3D11RenderTargetView* a_rtv)
	{
		if (a_name.empty())
			return nullptr;
		return Import(a_texture, fmt::format("cs.d3d11.{}", a_name),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeColour(a_desc, a_srv, a_rtv, a_uav); });
	}

	std::shared_ptr<org::ExternalTextureResource> ImportDepth(const RE::BSGraphics::DepthStencilData& a_target, const std::string& a_id)
	{
		return Import(a_target.texture, a_id, [&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeDepth(a_target, a_desc); });
	}

	void ReleaseImport(std::string_view a_id)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const auto found = state.imports.find(a_id);
		if (found == state.imports.end())
			return;
		// The graph resource views DXVK's image, whose memory the D3D11 texture owns: both go once the GPU is past every use,
		// which the graph's in-stream epochs and their compute joins precede.
		auto owner = std::make_shared<EngineImport>(std::move(found->second));
		state.imports.erase(found);
		Publish(state);
		Rebuild(state, ~0u, fmt::format("import '{}' released", a_id));
		RenderGraphRuntime::Get().ReleaseAfterD3D11(std::move(owner));
	}

	void OnEpochFailed(std::uint32_t a_epoch, const std::string& a_error, bool a_keptGraph)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const Anchor anchor = AnchorOfEpoch(a_epoch);
		if (anchor == Anchor::Count)
			return;
		const std::uint32_t index = IndexOf(anchor);
		const auto failed = state.builtSnapshots[index].load(std::memory_order_acquire);
		if (a_keptGraph) {
			// The previous graph still runs: what it was built from is the anchor's again.
			const auto previous = state.previousSnapshots[index].load(std::memory_order_acquire);
			state.builtSnapshots[index].store(previous, std::memory_order_release);
			state.built[index].store(previous ? previous->serial : 0, std::memory_order_release);
		}
		// The clients with passes at this anchor in the failed build, still registered as they were then. A client registered again
		// since is not in that build, and its rebuild is already asked for.
		std::vector<std::shared_ptr<const ClientDesc>> dropped;
		std::uint32_t mask = 1u << index;
		if (failed)
			for (const auto& client : failed->clients) {
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
			logger::error("[ORG] frame anchors: the {} epoch failed and none of its build's clients is still registered; it runs nothing until a "
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

	std::vector<std::pair<std::uint32_t, std::uint32_t>> RunsPerFrame()
	{
		const auto& state = Anchors();
		std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
		if (!RenderGraphRuntime::EpochsEnabled())
			return runs;
		for (std::uint32_t i = 0; i < kAnchors; ++i)
			if (state.reserved[i] && kInfo[i].runsPerFrame > 1)
				runs.emplace_back(RenderGraphRuntime::EpochOf(static_cast<Segment>(SegmentOf(static_cast<Anchor>(i)))), kInfo[i].runsPerFrame);
		return runs;
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
	std::uint32_t RunsPerFrame(Anchor) { return 0; }
	std::uint32_t PeakRunsPerFrame(Anchor) { return 0; }
	void FitSlotBudget(const std::vector<std::uint32_t>&, std::uint32_t, std::uint32_t) {}
	void OnFrameEnd() {}
	bool BuildPending(std::string_view) { return false; }
	void LogRunStats() {}
	std::uint64_t RenderOwnerChanges() { return 0; }
	std::shared_ptr<org::ExternalTextureResource> EngineTarget(std::uint32_t) { return nullptr; }
	std::shared_ptr<org::ExternalTextureResource> EngineDepth(std::uint32_t) { return nullptr; }
	std::shared_ptr<org::ExternalTextureResource> EngineCubemap(std::uint32_t) { return nullptr; }
	std::shared_ptr<org::ExternalTextureResource> D3D11Texture(std::string_view, ID3D11Texture2D*, ID3D11ShaderResourceView*, ID3D11UnorderedAccessView*, ID3D11RenderTargetView*) { return nullptr; }
	std::shared_ptr<org::ExternalTextureResource> ImportDepth(const RE::BSGraphics::DepthStencilData&, const std::string&) { return nullptr; }
	void ReleaseImport(std::string_view) {}
	bool InBuiltGraph(std::string_view, Anchor) { return false; }
	void Rearm(std::string_view) {}
	void SetLateEpochHook(std::function<void()>) {}
	std::uint32_t SegmentOf(Anchor) { return 0; }
	bool IsUnusedSegment(std::uint32_t) { return false; }
	const SegmentNames* NamesOf(std::uint32_t) { return nullptr; }
	void OnEpochFailed(std::uint32_t, const std::string&, bool) {}
	void AddToFrame(std::vector<std::uint32_t>&) {}
	std::vector<std::pair<std::uint32_t, std::uint32_t>> RunsPerFrame() { return {}; }
	void Attach(org::GraphHost&) {}
}

#endif

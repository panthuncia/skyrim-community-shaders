#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11UnorderedAccessView;
struct DxvkOrgInteropImageInfo;

namespace org
{
	class ExternalTextureResource;
	class GloballyIndexedResource;
	class GraphHost;
	class IResourceResolver;
	class RenderGraph;
	class RenderPass;
	class Resource;
	class VersionedTexture;
	struct TextureDescription;
}

namespace rhi
{
	enum class Format : std::uint32_t;
}

namespace RenderGraph
{
	class RunGate;
	namespace Frame
	{
		struct Description;
		enum class SegmentId : std::uint8_t;
	}
}

/**
 * @brief Named points in CS's frame where features attach render-graph passes: the frame anchors.
 *
 * Each anchor is one of CS's existing hook points (Deferred.cpp, Hooks.cpp, Upscaling.cpp). A feature reserves the anchors it
 * may use before the render graph initializes, then registers a client: its passes (compute, copy or raster, each declaring
 * what it reads and writes as any graph pass does), each with an anchor and an order key. At the hook, Run executes the anchor's
 * epoch: every registered pass of that anchor, in order-key order, at that point of the D3D11 stream.
 *
 * Cost: an anchor nobody reserved has no epoch at all (no graph, no run leases, nothing at the hook but one load). A reserved
 * anchor whose clients are all inactive this frame (or that has none) submits nothing at its hook.
 *
 * Mapping onto the runtime: each anchor is a RenderGraphRuntime::Segment of its own (Segment::Anchor*), an async live epoch of the
 * host (in DXVK's stream where the stream takes external commands). Its passes are tagged with that epoch. One host extension
 * ("cs.org.frame-anchors") makes every client's passes at each build; a client change rebuilds only its anchors' epochs. The ordering
 * rule of each anchor is in docs/development/org-extension-api-notes.md, "Attaching passes to the frame".
 *
 * Threads: everything here is called on the render thread (IsRenderOwner: the main thread, or with CS_ORG_LOADING_THREAD=1 the
 * thread that last began an engine frame). With that switch the state is also under a mutex, so a feature's Register from the
 * main thread during a load never interleaves with a loading-screen run. A pass's Prepare and Record run on the graph host's
 * thread, as for any graph pass, so they read only what they were made with or what the client's commit wrote into a LatchBlock.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
namespace FrameAnchors
{
	/**
	 * @brief A graph texture as passes declare it. Two kinds:
	 * - A slot: an import or an engine target ORG owns. It has one org::VersionedTexture for the id's life. A new D3D11 texture of the
	 *   same shape class (format, mip count, slice count) is the slot's next version, so nothing is built again.
	 * - A plain resource: anything else (a twin's Graph()), declared as itself.
	 *
	 * Passes hand it to the pass builder as they would a resource (`a_builder.ShaderResource(texture)`): ORG's SelfDeclaringResource
	 * hook declares the slot, or the plain resource. Never declare Current(): that is one version's backing, not the slot.
	 *
	 * Two handles are equal when they name the same slot (or the same plain resource), whatever version is current. So a client that
	 * compares its inputs to decide whether to register again does not register again for a new version.
	 *
	 * ORG-free: the out-of-line members live in FrameAnchors.cpp.
	 */
	class Texture
	{
	public:
		Texture() = default;
		Texture(std::nullptr_t) {}

		/** @brief A plain resource (a twin's Graph(), Drawcall Limit Fix's own import), declared as itself. */
		template <class T>
		Texture(std::shared_ptr<T> a_resource) :
			plain(std::move(a_resource))
		{}

		/** @brief A slot, named a_id in the logs. */
		Texture(std::string a_id, std::shared_ptr<org::VersionedTexture> a_slot);

		explicit operator bool() const { return slot || plain; }

		friend bool operator==(const Texture& a_left, const Texture& a_right) { return a_left.slot == a_right.slot && a_left.plain == a_right.plain; }

		/** @brief The slot; null for a plain resource. */
		const std::shared_ptr<org::VersionedTexture>& Slot() const { return slot; }

		/** @brief The plain resource; null for a slot. */
		const std::shared_ptr<org::Resource>& Plain() const { return plain; }

		/** @brief What stays the same across versions: the slot, or the plain resource. For a pass's Revision. */
		const void* Key() const { return slot ? static_cast<const void*>(slot.get()) : static_cast<const void*>(plain.get()); }

		/** @brief The import's graph id ("cs.engine.target.1", "Engine::kMAIN"); empty for a plain resource. */
		const std::string& Id() const { return id; }

		/**
		 * @brief The backing as it is now: the slot's current version, or the plain resource. For shape queries and raw handles outside the
		 * graph only. It changes with each version, so never declare it and never keep it across frames.
		 */
		std::shared_ptr<org::GloballyIndexedResource> Current() const;

		/** @brief Current(), for one call (`texture->GetAPIResource()`). */
		org::GloballyIndexedResource* operator->() const;

		/** @brief The current version's size and format (an ExternalTextureResource's or a PixelBuffer's); 0 or Unknown when there is none. */
		std::uint32_t Width() const;
		std::uint32_t Height() const;
		rhi::Format Format() const;

		/** @brief Whether both handles have the same current backing: a holder of raw handles of one version compares with this. */
		bool SameBacking(const Texture& a_other) const;

		/** @brief ORG's SelfDeclaringResource hook: the slot, or null for a plain resource. */
		const org::IResourceResolver* DeclaredResolver() const;

		/** @brief ORG's SelfDeclaringResource hook: the plain resource. */
		std::shared_ptr<org::Resource> DeclaredResource() const { return plain; }

	private:
		std::string id;
		std::shared_ptr<org::VersionedTexture> slot;
		std::shared_ptr<org::Resource> plain;
	};

	/** @brief The anchors, in the order a frame reaches them. */
	enum class Anchor : std::uint32_t
	{
		ReflectionsPrepass,  // BSCubeMapCamera::RenderCubemap, after the features' ReflectionsPrepass (Deferred::ReflectionsPrepasses)
		SkyReflections,      // a reflection face's sky batch (Drawcall Limit Fix's sky family), once per face drawn
		EarlyPrepass,        // Main::RenderShadowMaps returned, after the features' EarlyPrepass (Deferred::EarlyPrepasses)
		DepthDone,           // Main::RenderDepth returned (Terrain Blending's depth hook), before the features' Prepass
		Prepass,             // the main pass's first opaque batch, after the features' Prepass (Deferred::PrepassPasses)
		Sky,                 // the main pass's sky batch (Drawcall Limit Fix's sky family), after the opaque groups, before the decals
		DeferredPasses,      // the opaque pass done, after the final depth copy, before the D3D11 SSGI, SSS and composite
		DeferredComposite,   // after the D3D11 SSGI, SSS and cubemap update, where the deferred composite runs
		PostComposite,       // the end of the deferred passes, after the composite (D3D11 or ORG), where Effects11's rays run
		PostProcessing,      // Main_PostProcessing, before the D3D11 post-processing and the upscaler
		Upscale,             // Main_PostProcessing, after the upscaler's evaluate, before the ImageSpace chain
		Tonemap,             // the ImageSpace HDR tonemap's render, before Effects11 and Post Processing replace or follow it
		FrameGeneration,     // Main_PostProcessing, after the ImageSpace chain, where frame generation's inputs are captured
		Present,             // IDXGISwapChain::Present, before the overlay, HDR output and frame generation
		Overlay,             // the overlay's draw (OverlayRenderer::FinalizeImGuiFrame), after ImGui built its frame, before HDR output
		HDROutput,           // HDRDisplay's output composite (ApplyHDR), after the overlay and other mods' Present hooks, before the present
		Capture,             // the screenshot capture (ScreenshotFeature::ProcessCaptureRequest), after HDR output, before the native Present
		// The engine's own clears and copies on the graph (RenderGraph/Engine/Skeleton, CS_ORG_ENGINE_SKELETON). Each runs where the
		// engine would clear or copy, zero or more times a frame, so they are listed last, not in frame order.
		EngineShadowClear,  // a shadow map slice's clear (Renderer::ApplyState in BSShadowLight::RenderShadowmap)
		EngineCubeClear,    // a reflection face's clears (Renderer::ApplyState in BSCubeMapCamera::RenderFaces)
		EngineFrameClear,   // every other engine clear (ApplyState, SetDirtyStates, Renderer::ClearColor and ClearDepthStencil)
		EngineDepthCopy,    // the main depth copied into kPOST_ZPREPASS_COPY (Main::RenderDepth, FinishAccumulating, DeferredPasses)
		// Out of frame order (appended, so the values above stay): each Scaleform display's EndDisplay on the main thread, between
		// FrameGeneration and Present (RenderGraph/Engine/Scaleform), several times a frame.
		Interface,
		// Out of frame order (appended): the engine's ImageSpace draws the graph draws in the engine's place
		// (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE), a batch at a time, a few times a frame inside the ImageSpace chain.
		ImageSpace,
		// The engine's draw families drawn at their own call (Drawcall Limit Fix's water, precipitation, screen blood, shadow mask and
		// menu scene families): the world's shadow masks at Main::RenderShadowMasks, the water mask at FinishAccumulating's stencil batch,
		// the water at FinishAccumulatingPostResolveDepth's water batch, each rain or snow draw in the blended draws, a menu scene's
		// shadow masks and draws at UI3DSceneManager::Render, and the screen blood at Main::RenderScreenSplatter. Native mode: each
		// client is a named step of the stage its call falls in. Several runs a frame, so it is listed out of frame order.
		EngineDraws,
		// The engine's occlusion queries the graph answers (RenderGraph/Engine/Queries, CS_ORG_OCCLUSION_QUERIES): the sun's occlusion
		// draw between its BeginQuery and EndQuery (Drawcall Limit Fix's sky family), at Sun::SubmitOcclusionQuery's call, up to three
		// times a frame (the engine's three sun queries). A client of its own, so every run's queries are that client's.
		EngineQueries,
		// Out of frame order (appended): Volumetric Lighting's GPU work (Features/VolumetricLighting/ORGVolumetricLighting,
		// CS_ORG_VOLUMETRIC_LIGHTING), once a frame where the engine's VL blur would dispatch, inside RenderWorldPostProcess.
		VolumetricLighting,
		// Out of frame order (appended): the decal occlusion tests (Drawcall Limit Fix's decal occlusion family, CS_ORG_OCCLUSION_QUERIES),
		// one per decal the engine tests at the frame's start (SE 35563, AE 36562, before Main::Draw), only while decals wait to be
		// placed. Not EngineQueries: an inactive client's passes run in another client's runs, and their query reset and resolve would
		// overwrite counts its own last run left unread.
		DecalQueries,
		// Out of frame order (appended): native mode only (RenderGraph/Engine/Skeleton). The engine's copy of kMAIN into its copy
		// texture ("kMAIN Copy"), at the native immediate context's CopyResource, inside the ImageSpace chain. Once or twice a frame.
		EngineCopy,
		Count
	};

	/** @brief The kind of a pass: the graph's ExternalPassDesc::Compute, Copy or Render. */
	enum class PassKind : std::uint8_t
	{
		Compute,
		Copy,
		Render,
	};

	/** @brief The queue a pass prefers. Default: graphics for compute and copy (beside the D3D11 work around it), none for raster. */
	enum class PassQueue : std::uint8_t
	{
		Default,
		Compute,  // the graph's async compute queue: the anchor's later graphics work waits for it
	};

	/**
	 * @brief Where Run was called from: the anchor and, where the hook has them, its engine targets (RE::RENDER_TARGET values; the
	 * tonemap's input and output). -1 where the hook has none.
	 */
	struct Point
	{
		Anchor anchor = Anchor::Count;
		std::int32_t input = -1;
		std::int32_t output = -1;
	};

	/**
	 * @brief One pass of a client.
	 * - name: the graph pass's name, prefixed by the feature: "cs.ssgi.trace".
	 * - order: where it runs within its anchor, lowest first. Equal keys keep the clients' registration order, then each client's
	 *   own order. The graph still orders passes by what they declare: the key decides only between passes that do not depend on
	 *   each other's resources.
	 * - make: at each build of the anchor's graph, on the render thread: a new pass instance, or null to leave it out of that build.
	 * - once: a one-shot pass (a clear, an upload, a fill). It is in the anchor's graph until one run with it was submitted, then
	 *   the graph is built again without it; Rearm puts it back. The anchor runs while the pass is pending, active client or not.
	 *   Its work must give the same result if it ran twice.
	 * - atExecution: the pass is recorded after the run's commit instead of ahead on the graph host's thread
	 *   (ExternalPassDesc::RecordAtExecution): in DXVK's stream when the anchor's epoch runs there (guest), or inline into the
	 *   epoch's own command list at its submission (native, org::GraphHost::SetInlineAtExecution). A pass whose work this run is
	 *   known by then may skip the run (TypedRenderGraphPass's SkipsAtExecution: AnchorPasses' gated draws and dispatches whose gate
	 *   entry is closed). For anchors that run many gated passes of which a few are open each run. Ignored for a pass on the compute
	 *   queue, and when the epoch records nothing at execution (org::RenderGraph::PassEpochRecordsAtExecution).
	 */
	struct PassDesc
	{
		std::string name;
		Anchor anchor = Anchor::Count;
		std::int32_t order = 0;
		PassKind kind = PassKind::Compute;
		PassQueue queue = PassQueue::Default;
		std::function<std::shared_ptr<org::RenderPass>()> make;
		// In place of make, for a client with instances (ClientDesc::maxInstances above 1): the pass for one instance. A declared
		// segment makes it once per instance, named "<name>.i<instance>"; an anchor's own epoch makes instance 0 only (its hook runs
		// once per run there). Each instance reads its own gate entries and latch regions, which the commit of that instance's tap
		// writes (TapInstance).
		std::function<std::shared_ptr<org::RenderPass>(std::uint32_t a_instance)> makeInstance;
		bool once = false;
		bool atExecution = false;
		// With atExecution: every draw and dispatch of the pass takes its counts from its run gate (indirect), and it records no other
		// work. Recorded ahead, it does nothing on a run whose commit left its entries closed. So native mode records it ahead, in the
		// epoch's kept recording, when GPU gates are on (GpuGates). It is recorded at execution only to leave out the empty commands.
		bool gpuGated = false;
		// Declared segments: the pass's bit in its segment's declared rows (ClientDesc::rows; an engine effect step's pass, by its place
		// in its stage). Off in a recording whose description clears the bit: it records only its barriers. -1: no row.
		std::int16_t row = -1;
	};

	/**
	 * @brief A feature's passes and its hooks into the anchors' epochs. All callbacks run on the render thread.
	 * - id: unique, the feature's prefix: "cs.ssgi". Registering an id again replaces that client.
	 * - registerResources: at each build of a graph, before its passes are made: register the client's graph resources by name
	 *   (RenderGraph::RegisterResource), as an extension's PrepareForBuild does.
	 * - commit: inside the epoch, before its submission (RenderGraphRuntime::ExecuteEpoch's beforePrepare): write this execution's
	 *   values into the client's LatchBlock at the host's current run (GraphHost::CurrentRun), or queue uploads. Called for each of the client's
	 *   anchors that runs, once per run.
	 * - active: at the hook, before the epoch: whether the client has work this frame. When no client of an anchor is active, the
	 *   anchor submits nothing. Null: always active. An inactive client's passes still run when another client keeps the epoch,
	 *   with its commit not called: put its per-run dispatches and draws behind its run gates (gates), which the run closes.
	 * - onFailed: the graph failed and stopped, or the epoch of one of the client's anchors failed to build or prepare with the
	 *   client in it (OnEpochFailed). The client is unregistered; it returns to its D3D11 path.
	 * - sharedConstants: the client's passes read CS's shared constant buffers (SharedConstants.h). Each of its anchors' epochs
	 *   then writes their values at the hook and uploads them before the anchor's first pass. Such a client may register before
	 *   CS's first upload sized them: its passes join the graphs once the buffers can be made.
	 * - gates: the client's run gates (RunGate.h). Every run of one of its anchors closes them before any commit, so its gated
	 *   dispatches and draws do work only where this run's commit opened them. After Unregister (or a Register that replaced
	 *   them) the gates are still closed at every run of those anchors until their graphs are built without the old passes.
	 * - executed: after a run whose epoch was submitted with this registration's passes in its graph (built from this
	 *   registration or a later Refresh) and whose commit ran: what that commit wrote is in a submitted execution, and every
	 *   later GPU use of the client's resources is ordered after it. Render thread. The standard way to know a run reached
	 *   submission: count work done here (a regenerated table, a reset, a withheld draw that was drawn), never from commit,
	 *   frame counts or flags set in Prepare. A run that did not submit (any RunStatus but Ran) calls no executed.
	 */
	struct ClientDesc
	{
		std::string id;
		std::vector<PassDesc> passes;
		std::function<void(org::RenderGraph&)> registerResources;
		std::function<void(const Point&)> commit;
		std::function<bool()> active;
		std::function<void()> onFailed;
		bool sharedConstants = false;
		std::vector<std::shared_ptr<RenderGraph::RunGate>> gates;
		std::function<void(const Point&)> executed;
		// Declared segments (SegmentsOn): whether the client's passes do work in this frame. Read at the frame's start
		// (BeginFrame), from the client's settings and the scene state it holds, never from what this frame's hooks deliver. Off: its
		// passes record only their barriers in the segment's recording, and its commit is not called; an active client at the tap is
		// then a mismatch (Frame::Driver::NoteTapError), unless the frame's schedule lags its description (then its work waits for the
		// new schedule). Its per-run work is still behind its run gates. Null: on whenever registered.
		std::function<bool()> declared;
		// Declared segments: a client whose hook repeats inside one segment run (each ImageSpace effect call, each reflection face) has
		// instances. maxInstances is how many instances its passes are made for at a build (PassDesc::makeInstance); instances, read
		// at the frame's start like declared, is how many do work this frame. The segment's k-th tap of the client's step this frame
		// is instance k: its commit writes instance k's gate entries and latch regions (TapInstance), never the anchor's single set.
		// A tap past the declared count is a tap error and its commit is not called. Instances past the count record only their
		// barriers. A client without instances (maxInstances 1) may be tapped once per segment run.
		std::uint32_t maxInstances = 1;
		std::function<std::uint32_t()> instances;
		// Declared segments: the rows of the client's passes (PassDesc::row) that do work this frame, a bit per row, ORed into the
		// segment's description. Read at the frame's start only, from engine state there and what hooks asked for (a step whose row the
		// schedule has off draws nothing that frame and asks for its row). Null: every row on.
		std::function<void(std::array<std::uint64_t, 4>&)> rows;
		// Declared segments: the client's commit may run at several taps of one segment run, each writing gate entries and latch regions
		// no earlier tap of the run wrote (the stages' ImageSpace batches, one per ImageSpace scope). Without it a second tap of a client
		// without instances is a tap error.
		bool repeats = false;
		// Declared segments: the client's passes are only in the declared segments that hold it. The anchor's own graph leaves them out,
		// and the client may have work only while such a segment's window is open (CanTap); work outside it is a tap error and draws
		// nothing.
		bool segmentOnly = false;
		// Declared segments, with instances: the passes are made instance by instance (every pass of instance 0, then of instance 1, ...)
		// instead of pass by pass. The graph keeps declaration order between passes on one target, so the taps' order holds across passes
		// of different targets (Scaleform: each display draws onto one of several surfaces, and a later display draws over an earlier one).
		bool instanceMajor = false;
		// Declared segments: a registration of this id that the frame's schedule does not hold yet taps as the registration it holds (that
		// one's commit and executed callbacks). Only for a client whose commit writes state every registration's passes read (Scaleform: a
		// registration for new reads keeps its surfaces, slots and gate).
		bool matchesById = false;
		// The client's own count, returned by HeldNow: which registration the frame's schedule holds.
		std::uint64_t generation = 0;
	};

	/** @brief An anchor's name, as the logs and docs give it ("DeferredPasses"). */
	const char* Name(Anchor a_anchor);

	/**
	 * @brief Reserves an anchor's epoch. Call before the render graph initializes (a feature's Load or PostPostLoad, when its ORG
	 * path is on): the frame's epochs are fixed then. False, logged, afterwards.
	 */
	bool Reserve(Anchor a_anchor);

	/** @brief Whether the anchor has an epoch (reserved before initialization, and the runtime is up). */
	bool HasEpoch(Anchor a_anchor);

	/**
	 * @brief Whether a PassDesc::atExecution pass at the anchor is recorded after the run's commit (native: always; guest: when the epoch
	 * that runs it is in DXVK's stream, the stages' epochs for an anchor a stage shares). Such a pass can read what the commit wrote when
	 * it records, so its revision need not name the run: a ticket prepared ahead stays current. False for an anchor a stage holds wholly.
	 * Render thread, at a build (a pass's make).
	 */
	bool RecordsAtExecution(Anchor a_anchor);

	/**
	 * @brief Whether passes marked PassDesc::gpuGated are recorded ahead instead of at execution: native mode, unless
	 * CS_ORG_GPU_GATES=0. Their gate then decides on the GPU, through their indirect counts.
	 */
	bool GpuGates();

	/**
	 * @brief The Revision of a pass whose Record reads what its run's commit stored (a run slot): recorded at execution
	 * (a_atExecution, RecordsAtExecution at the build), nothing changes it, so a ticket prepared ahead stays current. Recorded ahead,
	 * every call gives a new word: the ticket is prepared again after the commit, which is when its Record can read the slot.
	 */
	inline void CommitReadRevision(bool a_atExecution, std::vector<std::uint64_t>& a_out)
	{
		static std::atomic<std::uint64_t> ahead{ 0 };
		a_out.push_back(a_atExecution ? 0 : ahead.fetch_add(1, std::memory_order_relaxed) + 1);
	}

	/**
	 * @brief Registers (or replaces) a client. Render thread, with the runtime active. False, logged, when the runtime is not active,
	 * a pass names an anchor without an epoch, a pass has no name or no make, or a name repeats. The anchors' graphs are built again
	 * at their next execution. A client that reads the shared constants before CS's first upload is held until they can be made,
	 * then joins the graphs; when they cannot be made at all, its onFailed is called.
	 */
	bool Register(ClientDesc a_client);

	/**
	 * @brief Removes a client; its anchors' graphs are built again at their next build. Nothing when the id is unknown. Until then
	 * the old graphs still hold its passes: their runs close its gates (ClientDesc::gates) and call none of its callbacks.
	 */
	void Unregister(std::string_view a_id);

	/**
	 * @brief The client's anchors' graphs built again at their next execution, with its passes and resources as they are then: after
	 * the client replaced a resource it registers outside GraphResources. Render thread. Nothing when the id is unknown. A
	 * GraphResources twin's Resize needs no call: the anchors see its generation at their next run and build again themselves.
	 */
	void Refresh(std::string_view a_id);

	/**
	 * @brief Whether a_anchor's graph, as last built, holds this registration of client a_id: its next run submits the client's
	 * passes. A client registered or refreshed since then is not in it yet. Render thread.
	 */
	bool InBuiltGraph(std::string_view a_id, Anchor a_anchor);

	/**
	 * @brief Whether a run of a_anchor now would draw a_id's work: the one question a client asks before it takes the engine's work. For
	 * an anchor a CS stage holds wholly (a whole step in the recipe table), the schedule of the stage that holds the anchor in this
	 * frame's kind holds this registration of a_id and has it on (false while the stage is absent, and when no stage of the frame's kind
	 * holds the anchor); that does not depend on where the frame is now, so a caller may ask before the stage's window opens. For a shared
	 * anchor, the open stage whose step takes the anchor's taps now holds a_id, and its schedule has it on (HeldNow). An anchor with an
	 * epoch of its own (the stages off): its own built graph holds this registration. Render thread. Run can still fail afterwards:
	 * the client's executed callback is how it knows the work was submitted.
	 */
	bool Scheduled(std::string_view a_id, Anchor a_anchor);

	/**
	 * @brief Whether the stage that holds anchor a_anchor wholly has client a_id's current registration in its schedule: the last frame's
	 * (or, before the first frame, its attached build). For a client that queues work before the frame's Begin (a Bink tick between two
	 * frames), where HeldNow's open window is not known yet. False when no stage holds the anchor. Render owner.
	 */
	bool InStage(std::string_view a_id, Anchor a_anchor);

	/** @brief HeldNow's answer. */
	struct StageHold
	{
		bool held = false;       // a run of the anchor now taps a CS stage (an open stage has a step there for this frame kind)
		bool scheduled = false;  // the frame's schedule of that stage holds a registration of the client (by id) and has it on
		std::uint64_t generation = 0;  // that registration's ClientDesc::generation
	};

	/**
	 * @brief Whether a run of a_anchor now would tap a CS stage that holds a_id at a step (rather than run the anchor's own epoch), and which
	 * registration of a_id the frame's schedule of it holds. A client that hands work to an anchor some stages hold asks this before it takes
	 * the engine's work: held and scheduled, the stage draws it; held and not scheduled, the work lags; not held, no stage has a place for
	 * it now (native mode: a mismatch). A client that changes what its passes draw onto compares the
	 * generation with its own: until the schedule holds the new one, its work lags. Render owner.
	 */
	StageHold HeldNow(std::string_view a_id, Anchor a_anchor);

	/** @brief The client's one-shot passes (PassDesc::once) pending again: each runs at its anchor's next run. Render thread. */
	void Rearm(std::string_view a_id);

	/**
	 * @brief The hook run after every submitted epoch of a late anchor (PostProcessing, Upscale, Tonemap, FrameGeneration, Present,
	 * Overlay, HDROutput, Capture): an upscaler, frame generation or the present may follow those directly. Upscaling installs its
	 * separation (Upscaling::SeparateFromGraphEpochs; render-graph.md, "Hazard: external Vulkan work directly after a graph
	 * epoch"). One hook; installing another replaces it.
	 */
	void SetLateEpochHook(std::function<void()> a_hook);

	/** @brief What a run did. */
	enum class RunStatus : std::uint8_t
	{
		Ran,       // the epoch was submitted; the executed callbacks of the clients in the built graph ran
		Idle,      // nothing to run: no client active and no one-shot pass pending, or the anchor has no epoch
		Refused,   // the caller does not own rendering (IsRenderOwner), or the runtime is not active
		Failed,    // the epoch failed (its clients are dropped at the next poll), or the graph stopped
	};

	/**
	 * @brief Run's result. Converts to true only for RunStatus::Ran, so a caller that only asks "did it submit" reads it as a bool.
	 * A caller that withheld the engine's work draws it through the engine for any other status.
	 */
	struct RunResult
	{
		RunStatus status = RunStatus::Idle;
		constexpr operator bool() const noexcept { return status == RunStatus::Ran; }
	};

	/**
	 * @brief The hook: runs the anchor's epoch at this point of the D3D11 stream when it has an active client or a pending one-shot
	 * pass. After the late anchors it runs the late-epoch hook (SetLateEpochHook). Render owner only, checked under the anchors'
	 * mutex. Each run takes a run lease of the host's, so a run never waits for another run of the same frame.
	 */
	RunResult Run(const Point& a_point);

	/** @brief Run for a hook without engine targets. */
	inline RunResult Run(Anchor a_anchor) { return Run(Point{ .anchor = a_anchor }); }

	/**
	 * @brief Whether the calling thread is inside Run (an anchor's epoch, its commits or its executed callbacks). Work that would run
	 * an anchor itself (native mode's immediate context taking an operation as a graph pass) checks this first: a run never starts
	 * another. Any thread.
	 */
	bool InRun();

	/**
	 * @brief True on the thread that owns rendering now: the only thread where anchors run (Run refuses elsewhere) and where per-frame
	 * work that feeds them (texture streaming's tick, Scaleform's capture, Bink's tick) may use the graph. A pure query: it never
	 * changes the owner. Any thread.
	 *
	 * Without CS_ORG_LOADING_THREAD=1 that is the engine's main thread alone: a loading screen (a job thread) and the startup logo
	 * (the MoviePlayer thread) stay on D3D11. With it, the owner is the thread that last began an engine frame (ClaimRenderOwnership,
	 * from Renderer::Begin): the main thread, the loading loop's thread or the logo's MoviePlayer thread. The engine begins every
	 * frame inside Renderer::Lock, on one thread at a time. docs/development/p5/loading-screen.md has the rule and its hazards. A
	 * false answer is always safe: the caller keeps its D3D11 or engine path for that call.
	 */
	bool IsRenderOwner();

	/**
	 * @brief The one claim point (CS_ORG_LOADING_THREAD=1; nothing otherwise): the calling thread owns rendering from now on. Called
	 * by the Renderer::Begin detour (SE 75460, AE 77245; RenderGraph/Engine/LoadingScreen) at the start of every engine frame, on the
	 * thread that renders it. Moves the graph's frame upload instance with it (RenderGraphRuntime::MoveOwnerThread).
	 */
	void ClaimRenderOwnership();

	/** @brief How many times rendering changed threads so far (CS_ORG_LOADING_THREAD=1). Any thread. */
	std::uint64_t RenderOwnerChanges();

	/** @brief Records the calling thread as the engine's main thread. SKSEPlugin_Load calls it: SKSE loads plugins there. */
	void LatchMainThread();

	/**
	 * @brief The engine's render target (RE::RENDER_TARGET) as a graph texture: one slot for the id's life. A new engine texture of the
	 * same format, mip count and slice count is the slot's next version, with no graph build; another shape class is a new slot, built
	 * in off the render thread. Its views follow the engine's (SRV, RTV, UAV where the engine made one). Registered with every graph as
	 * "cs.engine.target.<index>". Render thread (it synchronizes with DXVK's worker the first time). Null when the target does not
	 * exist or cannot be imported. Every mip and slice is described (a cube's views are cube views). When ORG owns the target
	 * (CS_ORG_ENGINE_TARGETS, RenderGraph/Engine/Targets), this and every other import here return its published texture
	 * ("Engine::kMAIN" and the like) instead, the one resource DCLF reads too.
	 *
	 * One image, one graph resource: every import here and ShareImage find an image's existing slot before making one. A second id
	 * for an image already imported shares its slot; an importer that needs a view the slot lacks makes a new version of the same
	 * image with both sets of views. In native mode the image's graph-owned texture (Engine::Native::GraphTextureOf) is always the
	 * backing: it has every view its bind flags allow.
	 *
	 * An image ORG made that already has a graph resource (RenderGraph::MadeImageOf: a GraphResources twin's D3D11 export, an engine
	 * texture, a streaming window, native mode's exempt textures) is not imported: the import returns that resource, or the twin's
	 * slot when it is versioned, and the id is not registered for it. Only when that resource lacks a view asked for is the image
	 * imported again (logged; the duplicate check names it).
	 */
	Texture EngineTarget(std::uint32_t a_target);

	/** @brief EngineTarget for a depth-stencil target (RE::RENDER_TARGETS_DEPTHSTENCIL), with its DSV and depth SRV: "cs.engine.depth.<index>". */
	Texture EngineDepth(std::uint32_t a_depthStencil);

	/**
	 * @brief EngineTarget for a cubemap target (RE::RENDER_TARGETS_CUBEMAP): a cube texture whose default SRV is a cube view, with its
	 * faces' RTVs: "cs.engine.cubemap.<index>".
	 */
	Texture EngineCubemap(std::uint32_t a_cubemap);

	/**
	 * @brief EngineTarget for a texture a feature still owns in D3D11 (one not ported yet), with the views given (any may be null; an
	 * RTV makes it a render target the graph can draw into): "cs.d3d11.<name>". A new a_texture of the same shape class is the slot's
	 * next version. A cube texture
	 * (D3D11_RESOURCE_MISC_TEXTURECUBE) gets cube views. The first import of a name decides its views: import it the same way everywhere.
	 */
	Texture D3D11Texture(std::string_view a_name, ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_srv,
		ID3D11UnorderedAccessView* a_uav, ID3D11RenderTargetView* a_rtv = nullptr);

	/**
	 * @brief A texture loaded once from a file and only read by shaders after (a noise or LUT texture, a bokeh shape, a default
	 * cubemap, an effect's file texture), with its SRV: "cs.d3d11.<name>", as D3D11Texture imports it. The one place such textures
	 * enter the graph. In native mode a texture made with its contents and only read is exempt (org::Resource::GraphOwnership::
	 * ExternalImmutableShaderResource, RenderGraph::Engine::Native::ImmutableTextureOf): one graph resource per texture, uploaded and
	 * left in the shader-read state before any graph reads it, which passes may only read and graphs do not track. It is not
	 * registered under the name. Anything else (and every texture in guest mode) imports as D3D11Texture does.
	 */
	Texture ImmutableTexture(std::string_view a_name, ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_srv);

	/**
	 * @brief EngineDepth for a depth texture outside the engine's table (a feature's own map, or one only a view of reaches the
	 * feature): a_target's texture with its DSV and depth SRV, either of which may be null. Registered with every graph as a_id, and
	 * kept while a_target's texture stays the same. Null when the texture cannot be imported.
	 */
	Texture ImportDepth(const RE::BSGraphics::DepthStencilData& a_target, const std::string& a_id);

	/**
	 * @brief EngineTarget for any other D3D11 texture, with the views a_views names (its format, view flags and view formats; the
	 * subresources come from the image): "a_id". For a target a client draws onto that is neither an engine target nor a feature
	 * texture with D3D11 views (Scaleform's and ImGui's other targets). Registered with every graph as a_id.
	 */
	Texture ImportTexture(std::string_view a_id, ID3D11Texture2D* a_texture, const org::TextureDescription& a_views);

	/**
	 * @brief The one graph resource of an image that a graph outside the anchors (Drawcall Limit Fix's epochs) uses: the engine
	 * target's slot when ORG owns it, else the slot any importer already made for the image (an id here, or an earlier ShareImage),
	 * else a new slot. a_texture is the image's D3D11 texture: the slot keeps it alive, so the image handle is never reused while the
	 * slot lives. a_desc names the views the caller needs; a slot that lacks one gets a new version of the same image with both
	 * sets of views. A slot made here is not registered under an id and lives while its holders hold it; an id import of the same
	 * image later takes it over. An image ORG made with a graph resource of its own gives that resource, as the imports above do.
	 * Native mode: the image's graph-owned texture is the backing. Render thread.
	 * Empty when the render graph is not active or the image cannot be imported.
	 */
	Texture ShareImage(ID3D11Texture2D* a_texture, const DxvkOrgInteropImageInfo& a_image, const org::TextureDescription& a_desc, std::string_view a_name);

	/**
	 * @brief Drops an import by its graph id ("cs.d3d11.<name>" for D3D11Texture, a_id for ImportDepth): the D3D11 texture is
	 * released once D3D11's last use is done (ReleaseAfterD3D11) and every anchor graph is built again without it (a structural
	 * build, off the render thread). For a feature
	 * texture that is destroyed or replaced under another name. Render thread. Nothing when the id is unknown.
	 */
	void ReleaseImport(std::string_view a_id);

	/**
	 * @brief Whether the declared frame's stages are on (docs/development/org-native-frame-graph.md, "The recipe"): native mode unless
	 * CS_ORG_SEGMENTS=0, with epochs, async epochs and replayable recordings. Each CS stage is one graph, revision-driven,
	 * recorded on the host's thread for each description of it (RenderGraph::Frame::Description) and kept per frame slot. Its steps are
	 * hooks in frame order (Frame/RecipeTable.h): anchors it holds wholly (which then have no epoch of their own), named clients of shared
	 * anchors, and other modules' epochs (SetForeignPasses). A tap stages its clients' values into the frame slot's staging run (the frame inputs); no
	 * ticket is taken there. The stage's commit point (its last step) copies the staged values into the slot's copy and submits the one
	 * ticket. Stages: pre-depth (EarlyPrepass), pre-main (RenderDepth's depth copy to Prepass; CS_ORG_SEGMENT_PREMAIN=0 off), deferred
	 * and water (DeferredPasses to just before the forward epoch; CS_ORG_SEGMENT_DEFERRED=0 off), and the post chain's two
	 * (CS_ORG_SEGMENT_POST=0 off): the post stage, with the chain and the upscaler's evaluate as CS passes. Guest mode (DXVK with the
	 * graph) runs the same stages, each one block in DXVK's stream. Any thread.
	 */
	bool SegmentsOn();

	/**
	 * @brief Whether a CS stage holds staged values it has not committed yet: its window is open. D3D11 work that reaches DXVK now runs
	 * ahead of the stage's block on the GPU (guest mode's residue census counts it). Any thread, without the anchors' lock.
	 */
	bool StagesHoldValues();

	/**
	 * @brief The CS stages (a bit per RenderGraph::Frame::SegmentId value) that hold staged values now, for the residue census to name the
	 * window a call came in. Any thread, without the anchors' lock.
	 */
	std::uint32_t StagesHoldingValues();

	/**
	 * @brief Called on the render owner each time the last open stage window closes (a commit, or staged values dropped): what was kept from
	 * DXVK inside the windows is applied then (the flush latch's write-behind of the engine's constant buffers, Engine/Flush). One hook.
	 */
	void SetWindowsClosedHook(void (*a_hook)());

	/**
	 * @brief Render owner: whether the description being read is a stage's build key (docs/development/org-native-frame-graph.md,
	 * "Absent passes"), not a frame's. A client's rows callback then sets every row a frame may pick with the settings now (both
	 * parities, every target variant), the rows a hook asked for, and the rows that must always be present. A pass the key leaves out is
	 * absent from the stage's build.
	 */
	bool DescribingKey();

	/**
	 * @brief Render owner, inside a client's rows callback while a frame is described: whether the build the frame is described against
	 * has row a_row of that client present. Empty when that is not known (a key being described, no key, a build with every pass): the
	 * caller decides from the frame. For a choice the build fixes (a pass's targets), so the frame describes what its build can run.
	 */
	std::optional<bool> DescribedBuildHasRow(std::uint32_t a_row);

	/** @brief Whether the calling thread is the engine's main thread (LatchMainThread). Any thread. */
	bool IsMainThread();

	/**
	 * @brief Inside a client's commit at a declared segment's tap: the instance the commit writes, that is how many earlier taps of
	 * the same step the segment's run had this frame (0 for the first). A client with instances (ClientDesc::maxInstances) opens
	 * instance k's gate entries and writes instance k's latch regions (InstanceEntry). 0 outside a segment's commit. Render owner.
	 */
	std::uint32_t TapInstance();

	/**
	 * @brief Whether a tap of client a_id now would commit into a declared segment: a segment that holds the client has its window open
	 * (the frame declares it, its end tap has not fired, the segments before it ended) and its built graph holds this registration.
	 * A segment-only client (ClientDesc::segmentOnly) takes work only then. Render owner.
	 */
	bool CanTap(std::string_view a_id);

	/**
	 * @brief Whether work of client a_id that the frame's schedule leaves out may lag (Frame::Driver::NoteStepLag): a build that holds its
	 * registration is on its way (asked for, building or staged), its stage has no schedule this frame or runs an older description while
	 * the frame's own is recorded, its stage's window is not open, or the build the stage runs holds it (the row is off: the next
	 * description turns it on). False when the stage runs the schedule of its current description on a build without the client and no
	 * build of the client is coming: that work would never be drawn. Render owner.
	 */
	bool TapMayLag(std::string_view a_id);

	/**
	 * @brief Whether a declared segment (a CS stage) holds anchor a_anchor wholly: every run of it is one of the stage's taps, and it has
	 * no epoch of its own. Fixed once the render graph initialized. Any thread.
	 */
	bool HeldByStage(Anchor a_anchor);

	/**
	 * @brief Whether CS stage a_segment (a RenderGraph::Frame::SegmentId value) is open now: the frame declares it, it has not ended, and the
	 * stages before it have (WindowOpen). Native mode, render owner; false otherwise.
	 */
	bool StageOpen(std::uint32_t a_segment);

	/**
	 * @brief Whether CS stage a_segment is declared by the frame and has no schedule yet (its first build), and has not ended: its work this
	 * frame lags. Native mode, render owner; false otherwise.
	 */
	bool StageAbsent(std::uint32_t a_segment);

	/**
	 * @brief Drops the finished recordings the stages that hold a_anchor wholly keep for reuse. A late batch keeps what its last
	 * recording retained (a movie's staging buffers) until it is recorded again, and a kept recording of a description that does
	 * not come back is never recorded again. Call when such work ends (Bink's movie). A frame slot's schedule keeps the recording
	 * it runs on until it switches; a description seen again is recorded again. a_why is logged. Render owner.
	 */
	void DropKeptRecordings(Anchor a_anchor, std::string_view a_why);

	/**
	 * @brief Native mode with CS's stages: for a client with instances at an anchor a stage holds wholly, the instance whose passes come first
	 * in the stage's graph (the first step at the anchor makes its range from there; the UI stage's Interface steps make 8 to 15, then 0 to 7).
	 * No earlier tap of the frame can have written anything that instance's passes read. Empty without stages, or when no stage holds the
	 * anchor wholly. Any thread, after the render graph initialized.
	 */
	std::optional<std::uint32_t> FirstGraphInstance(Anchor a_anchor);

	/** @brief Whether a declared segment (a CS stage) names client a_id at one of its steps. Fixed once the render graph initialized. Any thread. */
	bool NamedByStage(std::string_view a_id);

	/**
	 * @brief Whether a CS stage has a step at a_anchor (the recipe table), with the stages on (SegmentsOn). Answers before the anchors are
	 * reserved, so a module can reserve an anchor only where a stage uses it. Any thread.
	 */
	bool StageStepsAt(Anchor a_anchor);

	/**
	 * @brief Writes frame inputs for client a_id outside its step's tap: a_write runs with GraphHost::CurrentRun the frame slot's staging
	 * run of the stage that holds the client, while that stage's window is open and the frame's schedule has the client on. What it
	 * writes (gate entries, latch values) reaches the GPU with the stage's one commit. For data the engine hands over at a call that is
	 * not the step's hook (the renderer clear colour at its clear call). False, and a_write not called, otherwise. Render owner.
	 */
	bool StageInputs(std::string_view a_id, const std::function<void()>& a_write);

	/**
	 * @brief Where in an open stage work the engine hands over outside a tap belongs (a clear: the UI stage clears at three places). Of the
	 * stage's steps that name one of a_ids, the latest at most one step after the stage's last tapped step this frame, else the first of them:
	 * the index in a_ids of the client that step names. -1 when no open stage names any of them. Hand the work to that client with StageInputs.
	 * Render owner.
	 */
	std::int32_t StagePlace(std::span<const std::string_view> a_ids);

	/**
	 * @brief A CS point that ends CS stage a_segment and is no anchor (its point step: RenderEndOfFrameEffects' return ends the post
	 * stage). The recipe table names each stage's commit sites (Frame/RecipeTable.h, Stage::commitSite). Outside the stage's window it
	 * does nothing (the stage did not open this frame, or it is not declared). Render owner.
	 */
	RunResult TapPoint(RenderGraph::Frame::SegmentId a_segment);

	/** @brief The gate entry (or latch region index) of a_entry for instance a_instance, a_perInstance entries an instance. */
	constexpr std::uint32_t InstanceEntry(std::uint32_t a_instance, std::uint32_t a_perInstance, std::uint32_t a_entry)
	{
		return a_instance * a_perInstance + a_entry;
	}

	/**
	 * @brief The segment (a RenderGraphRuntime::Segment value) whose epoch an anchor's runs submit: its own, or the declared segment
	 * that holds it (SegmentsOn). Any thread, after Initialize.
	 */
	std::uint32_t RunSegment(Anchor a_anchor);

	/**
	 * @brief The frame driver, at Renderer::Begin (RenderGraph/Frame/FrameDriver.h): each declared segment takes its frame slot's
	 * schedule, the description and recording its frames run on to the frame's end. The frame's own description is made from the
	 * clients' declared predicates and the segments' built graphs; when it differs from the schedule, its recordings are asked for on
	 * the host's thread, and the schedule switches to it at the first Begin of the slot after they are ready. Nothing waits. A
	 * segment with no schedule on the attached build is absent from the frame (a_frame's fields with graph 0). Off the render owner,
	 * and with segments off, nothing is declared.
	 */
	void BeginFrame(RenderGraph::Frame::Description& a_frame);

	/** @brief Initialize: the declared segments' epochs, which the host makes revision-driven and closed. Empty with segments off. */
	std::vector<std::uint32_t> SegmentEpochs();

	/** @brief SegmentEpochs with each epoch's segment (a RenderGraph::Frame::SegmentId value), for the frame arena's spans. */
	std::vector<std::pair<std::uint8_t, std::uint32_t>> SegmentEpochsById();

	/**
	 * @brief Another module's epoch whose passes a declared segment holds (a_segment, a RenderGraphRuntime::Segment value: Light
	 * Limit Fix's light culling, held by the pre-main segment). The module gives:
	 * - a_gather: at each build of the segment's graph, appends its passes to a_passes, which is a
	 *   `std::vector<org::RenderGraph::ExternalPassDesc>*` (this header stays free of ORG includes), each tagged with a_epoch.
	 * - a_declared: at the frame's start, whether its passes do work this frame (from the state the module holds).
	 * The segment's graph is built again with them. Render thread. Nothing when no segment holds a_segment (HoldsForeign).
	 */
	void SetForeignPasses(std::uint32_t a_segment, std::function<void(std::uint32_t a_epoch, void* a_passes)> a_gather, std::function<bool()> a_declared);

	/** @brief Whether a declared segment holds the module epoch a_segment's passes: the module then gathers none itself. Any thread, after Initialize. */
	bool HoldsForeign(std::uint32_t a_segment);

	/**
	 * @brief The module epoch a_segment's hook, when a declared segment holds it (HoldsForeign): a_commit writes its run's values
	 * (GraphHost::CurrentRun is the segment's run), and the segment submits them at its end tap. A hook outside a frame whose
	 * description holds the segment is a tap error and draws nothing. Render owner.
	 */
	RunResult TapForeign(std::uint32_t a_segment, const std::function<void()>& a_commit);

	/**
	 * @brief GPU work is about to be submitted (a_why, a static string; a_segment: the epoch about to be submitted, if one). The stages
	 * whose commit point it is commit first (the start, pre-main, deferred+water, sun and UI stages), and work submitted while a stage
	 * holds staged values is counted for the census. Render owner only. RenderGraphRuntime calls it before every epoch, external epoch
	 * and external commands and at build points; the present hook calls it before the present.
	 */
	void BeforeGpuWork(const char* a_why, std::uint32_t a_segment = UINT32_MAX);

	// What RenderGraphRuntime asks of the anchors.

	/** @brief The anchor's segment (RenderGraphRuntime::Segment, as its value). */
	std::uint32_t SegmentOf(Anchor a_anchor);

	/** @brief Whether a segment value is an anchor's that has no epoch (no reservation): the runtime leaves it out everywhere. */
	bool IsUnusedSegment(std::uint32_t a_segment);

	/** @brief An anchor segment's names: its label (epoch events, logs), its inputs label, the profiler's prefix and the contributor id. */
	struct SegmentNames
	{
		const char* label;
		const char* inputs;
		const char* profiler;
		const char* contributor;
	};
	const SegmentNames* NamesOf(std::uint32_t a_segment);

	/**
	 * @brief Setup's build point (the primary path, docs/development/p5/primary-path.md rule 3): on the render owner, between frames,
	 * the graphs that registered clients wait to join are built now, so those clients are in their anchors' graphs before the first
	 * frame that needs them and never wait for a build at their first tap. a_caller names the caller in the log. Returns whether every registered client
	 * is in its anchors' built graphs afterwards; false (logged) off the render owner, with the runtime inactive, or when a build failed.
	 * After the first world frame nothing is built here: the builds are handed to the build manager, the clients join when they are
	 * attached, and this returns false.
	 */
	bool BuildPending(std::string_view a_caller);

	/**
	 * @brief After setup's build point, before the first frame: the UI stage's recordings of the first frames' description (a Bink movie or
	 * the main menu) are made on the host's thread for every frame slot and installed as their schedule, so the first frame draws the
	 * stage (the session's first movie frame). Setup waits for them here; no frame does. Render owner.
	 */
	void PrimeStages(std::string_view a_caller);

	/** @brief Logs each reserved anchor's peak runs a frame (CS_ORG_EPOCH_STATS's report). Render thread. */
	void LogRunStats();

	/**
	 * @brief Initialize, for the host's epoch order and again for its async epochs: the reserved anchors' epochs inserted into
	 * a_epochs (epoch ids, which are segment values) before the first epoch the frame runs after them. The first call freezes the
	 * reservations.
	 */
	void AddToFrame(std::vector<std::uint32_t>& a_epochs);

	/**
	 * @brief Initialize, after Attach and before the host's async epochs start: every reserved anchor's epoch keeps several tickets
	 * prepared (GraphHost::SetEpochTicketsAhead), as many as it ran the frame before, so an anchor that runs many times a frame
	 * back to back finds each run's ticket ready. Safe because an anchor's passes read only what they were made with or what a
	 * commit wrote into a LatchBlock, never what a commit changes. CS_ORG_TICKETS_AHEAD sets the most (16; 1 turns it off).
	 */
	void SetTicketsAhead(org::GraphHost& a_host);

	/**
	 * @brief Render thread (RenderGraphRuntime::PollEpochFailures): an anchor's epoch failed to build or prepare. Every client with
	 * passes at that anchor in the failed build is unregistered, its onFailed called, and the anchor (and the dropped clients' other
	 * anchors) built again without them. One error line names the clients and the error. a_keptGraph: the build failed and the
	 * anchor's previous graph still runs. Nothing for an epoch that is not an anchor's.
	 */
	void OnEpochFailed(std::uint32_t a_epoch, const std::string& a_error, bool a_keptGraph);

	/** @brief Initialize, once, with the host made and no graph built: adds the anchors' extension when any anchor has an epoch. */
	void Attach(org::GraphHost& a_host);
}

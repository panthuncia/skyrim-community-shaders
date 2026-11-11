#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11UnorderedAccessView;

namespace org
{
	class ExternalTextureResource;
	class GraphHost;
	class RenderGraph;
	class RenderPass;
}

namespace RenderGraph
{
	class RunGate;
}

/**
 * @brief Named points in CS's frame where features attach render-graph passes: the frame anchors.
 *
 * Each anchor is one of CS's existing hook points (Deferred.cpp, Hooks.cpp, Upscaling.cpp). A feature reserves the anchors it
 * may use before the render graph initializes, then registers a client: its passes (compute, copy or raster, each declaring
 * what it reads and writes as any graph pass does), each with an anchor and an order key. At the hook, Run executes the anchor's
 * epoch: every registered pass of that anchor, in order-key order, at that point of the D3D11 stream.
 *
 * Cost: an anchor nobody reserved has no epoch at all (no graph, no frame slots, nothing at the hook but one load). A reserved
 * anchor whose clients are all inactive this frame (or that has none) submits nothing at its hook.
 *
 * Mapping onto the runtime: each anchor is a RenderGraphRuntime::Segment of its own (Segment::Anchor*), an async live epoch of the
 * host (in DXVK's stream where the stream takes external commands). Its passes are tagged with that epoch. One host extension
 * ("cs.org.frame-anchors") makes every client's passes at each build; a client change rebuilds only its anchors' epochs. The ordering
 * rule of each anchor is in docs/development/org-extension-api-notes.md, "Attaching passes to the frame".
 *
 * Threads: everything here is called on the render thread (OnRenderThread: the main thread, or with CS_ORG_LOADING_THREAD=1 the
 * thread that renders under Renderer::Lock). With that switch the state is also under a mutex, so a feature's Register from the
 * main thread during a load never interleaves with a loading-screen run. A pass's Prepare and Record run on the graph host's
 * thread, as for any graph pass, so they read only what they were made with or what the client's commit wrote into a LatchBlock.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
namespace FrameAnchors
{
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
	 */
	struct PassDesc
	{
		std::string name;
		Anchor anchor = Anchor::Count;
		std::int32_t order = 0;
		PassKind kind = PassKind::Compute;
		PassQueue queue = PassQueue::Default;
		std::function<std::shared_ptr<org::RenderPass>()> make;
		bool once = false;
	};

	/**
	 * @brief A feature's passes and its hooks into the anchors' epochs. All callbacks run on the render thread.
	 * - id: unique, the feature's prefix: "cs.ssgi". Registering an id again replaces that client.
	 * - registerResources: at each build of a graph, before its passes are made: register the client's graph resources by name
	 *   (RenderGraph::RegisterResource), as an extension's PrepareForBuild does.
	 * - commit: inside the epoch, before its submission (RenderGraphRuntime::ExecuteEpoch's beforePrepare): write this execution's
	 *   values into the client's LatchBlock at the host's current frame slot, or queue uploads. Called for each of the client's
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
	 *   dispatches and draws do work only where this run's commit opened them.
	 * - executed: after a run whose epoch was submitted with this registration's passes in its graph (built from this
	 *   registration or a later Refresh) and whose commit ran: what that commit wrote is in a submitted execution, and every
	 *   later GPU use of the client's resources is ordered after it. Render thread. The signal for work a client counts done
	 *   once it ran (a regenerated table, a reset), in place of frame counts or flags set in Prepare.
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
	};

	/** @brief An anchor's name, as the logs and docs give it ("DeferredPasses"). */
	const char* Name(Anchor a_anchor);

	/**
	 * @brief Reserves an anchor's epoch. Call before the render graph initializes (a feature's Load or PostPostLoad, when its ORG
	 * path is on): the frame's epochs are fixed then. False, logged, afterwards, or when epochs are off (CS_ORG_EPOCHS=0).
	 */
	bool Reserve(Anchor a_anchor);

	/** @brief Whether the anchor has an epoch (reserved before initialization, and the runtime is up). */
	bool HasEpoch(Anchor a_anchor);

	/**
	 * @brief Registers (or replaces) a client. Render thread, with the runtime active. False, logged, when the runtime is not active,
	 * a pass names an anchor without an epoch, a pass has no name or no make, or a name repeats. The anchors' graphs are built again
	 * at their next execution. A client that reads the shared constants before CS's first upload is held until they can be made,
	 * then joins the graphs; when they cannot be made at all, its onFailed is called.
	 */
	bool Register(ClientDesc a_client);

	/** @brief Removes a client; its anchors' graphs are built again at their next execution. Nothing when the id is unknown. */
	void Unregister(std::string_view a_id);

	/**
	 * @brief The client's anchors' graphs built again at their next execution, with its passes and resources as they are then: after
	 * the client replaced a resource it registers outside GraphResources. Render thread. Nothing when the id is unknown. A
	 * GraphResources twin's Resize needs no call: the anchors see its generation at their next run and build again themselves.
	 */
	void Refresh(std::string_view a_id);

	/**
	 * @brief Whether a_anchor's graph, as last built, holds this registration of client a_id: its next run submits the client's
	 * passes. A client registered or refreshed since then is not in it yet (the graph is built again on the host's thread).
	 * Render thread.
	 */
	bool InBuiltGraph(std::string_view a_id, Anchor a_anchor);

	/** @brief The client's one-shot passes (PassDesc::once) pending again: each runs at its anchor's next run. Render thread. */
	void Rearm(std::string_view a_id);

	/**
	 * @brief The hook run after every submitted epoch of a late anchor (PostProcessing, Upscale, Tonemap, FrameGeneration, Present,
	 * Overlay, HDROutput, Capture): an upscaler, frame generation or the present may follow those directly. Upscaling installs its
	 * separation (Upscaling::SeparateFromGraphEpochs; render-graph.md, "Hazard: external Vulkan work directly after a graph
	 * epoch"). One hook; installing another replaces it.
	 */
	void SetLateEpochHook(std::function<void()> a_hook);

	/**
	 * @brief The hook: runs the anchor's epoch at this point of the D3D11 stream when it has an active client or a pending one-shot
	 * pass. After the late anchors it runs the late-epoch hook (SetLateEpochHook).
	 * @return Whether an epoch was submitted.
	 */
	bool Run(const Point& a_point);

	/** @brief Run for a hook without engine targets. */
	inline bool Run(Anchor a_anchor) { return Run(Point{ .anchor = a_anchor }); }

	/**
	 * @brief True on the thread that owns rendering now: the only thread where anchors run (Run returns false elsewhere) and where
	 * per-frame work that feeds them (texture streaming's tick, Scaleform's capture, Bink's tick) may use the graph.
	 *
	 * Without CS_ORG_LOADING_THREAD=1 that is the engine's main thread alone: a loading screen (a job thread) and the startup logo
	 * (the MoviePlayer thread) stay on D3D11. With it, one thread owns rendering at a time, the main thread at the start: a thread
	 * that holds Renderer::Lock takes ownership (the engine renders every frame inside that lock, on one thread at a time), and the
	 * main thread also takes it while no other thread holds the lock. A claim moves the graph's frame upload instance with it
	 * (RenderGraphRuntime::MoveOwnerThread). docs/development/p5/loading-screen.md has the rule and its hazards. A false answer is
	 * always safe: the caller keeps its D3D11 or engine path for that call.
	 */
	bool OnRenderThread();

	/**
	 * @brief The calling thread owns rendering from now on (CS_ORG_LOADING_THREAD=1; nothing otherwise). The loading-screen handoff
	 * calls it on the main thread once SetLoadingScreenActive(false) returned, when the loading loop has stopped.
	 */
	void TakeRenderOwnership();

	/** @brief How many times rendering changed threads so far (CS_ORG_LOADING_THREAD=1). Any thread. */
	std::uint64_t RenderOwnerChanges();

	/** @brief Records the calling thread as the engine's main thread. SKSEPlugin_Load calls it: SKSE loads plugins there. */
	void LatchMainThread();

	/**
	 * @brief The engine's render target (RE::RENDER_TARGET) as a graph texture, imported once and kept while the engine's texture
	 * stays the same. Its views follow the engine's (SRV, RTV, UAV where the engine made one). Registered with every graph as
	 * "cs.engine.target.<index>". Render thread (it synchronizes with DXVK's worker the first time). Null when the target does not
	 * exist or cannot be imported. Every mip and slice is described (a cube's views are cube views). A separate graph resource from Drawcall Limit Fix's import of the same image, which is safe because
	 * no anchor epoch is one of DCLF's. When ORG owns the target (CS_ORG_ENGINE_TARGETS, RenderGraph/Engine/Targets), this and every
	 * other import here return its published texture ("Engine::kMAIN" and the like) instead, the one resource DCLF reads too.
	 */
	std::shared_ptr<org::ExternalTextureResource> EngineTarget(std::uint32_t a_target);

	/** @brief EngineTarget for a depth-stencil target (RE::RENDER_TARGETS_DEPTHSTENCIL), with its DSV and depth SRV: "cs.engine.depth.<index>". */
	std::shared_ptr<org::ExternalTextureResource> EngineDepth(std::uint32_t a_depthStencil);

	/**
	 * @brief EngineTarget for a cubemap target (RE::RENDER_TARGETS_CUBEMAP): a cube texture whose default SRV is a cube view, with its
	 * faces' RTVs: "cs.engine.cubemap.<index>".
	 */
	std::shared_ptr<org::ExternalTextureResource> EngineCubemap(std::uint32_t a_cubemap);

	/**
	 * @brief EngineTarget for a texture a feature still owns in D3D11 (one not ported yet), with the views given (either may be null):
	 * "cs.d3d11.<name>". Imported again when a_texture changes. A cube texture (D3D11_RESOURCE_MISC_TEXTURECUBE) gets cube views.
	 */
	std::shared_ptr<org::ExternalTextureResource> D3D11Texture(std::string_view a_name, ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_srv,
		ID3D11UnorderedAccessView* a_uav);

	/**
	 * @brief EngineDepth for a depth texture outside the engine's table (a feature's own map, or one only a view of reaches the
	 * feature): a_target's texture with its DSV and depth SRV, either of which may be null. Registered with every graph as a_id, and
	 * kept while a_target's texture stays the same. Null when the texture cannot be imported.
	 */
	std::shared_ptr<org::ExternalTextureResource> ImportDepth(const RE::BSGraphics::DepthStencilData& a_target, const std::string& a_id);

	/**
	 * @brief Drops an import by its graph id ("cs.d3d11.<name>" for D3D11Texture, a_id for ImportDepth): the D3D11 texture is
	 * released once D3D11's last use is done (ReleaseAfterD3D11) and every anchor graph is built again without it. For a feature
	 * texture that is destroyed or replaced under another name. Render thread. Nothing when the id is unknown.
	 */
	void ReleaseImport(std::string_view a_id);

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
	 * @brief Initialize, for the host's epoch order and again for its async epochs: the reserved anchors' epochs inserted into
	 * a_epochs (epoch ids, which are segment values) before the first epoch the frame runs after them. The first call freezes the
	 * reservations.
	 */
	void AddToFrame(std::vector<std::uint32_t>& a_epochs);

	/**
	 * @brief Initialize, after AddToFrame: the reserved anchors that may run more than once a frame, each as its epoch and the most
	 * runs a frame its epoch is sized for (GraphHost::Desc::epochRuns). Each such epoch's ring of frame slots grows up to
	 * kGameFramesInFlight times its runs, so the host never waits for a run of the same frame (one DXVK has not submitted yet). A
	 * frame with more runs than the ring holds skips the rest (ExecuteEpoch returns false) instead of hanging.
	 */
	std::vector<std::pair<std::uint32_t, std::uint32_t>> RunsPerFrame();

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

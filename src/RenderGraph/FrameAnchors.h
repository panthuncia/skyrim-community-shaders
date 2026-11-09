#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace org
{
	class ExternalTextureResource;
	class GraphHost;
	class RenderGraph;
	class RenderPass;
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
 * Threads: everything here is called on the render thread. A pass's Prepare and Record run on the graph host's thread, as for any
 * graph pass, so they read only what they were made with or what the client's commit wrote into a LatchBlock.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
namespace FrameAnchors
{
	/** @brief The anchors, in the order a frame reaches them. */
	enum class Anchor : std::uint32_t
	{
		ReflectionsPrepass,  // BSCubeMapCamera::RenderCubemap, after the features' ReflectionsPrepass (Deferred::ReflectionsPrepasses)
		EarlyPrepass,        // Main::RenderShadowMaps returned, after the features' EarlyPrepass (Deferred::EarlyPrepasses)
		Prepass,             // the main pass's first opaque batch, after the features' Prepass (Deferred::PrepassPasses)
		DeferredPasses,      // the opaque pass done, after the final depth copy, before the D3D11 SSGI, SSS and composite
		PostProcessing,      // Main_PostProcessing, before the D3D11 post-processing and the upscaler
		Tonemap,             // the ImageSpace HDR tonemap's render, before Effects11 and Post Processing replace or follow it
		Present,             // IDXGISwapChain::Present, before the overlay, HDR output and frame generation
		Overlay,             // the overlay's draw (OverlayRenderer::FinalizeImGuiFrame), after ImGui built its frame, before HDR output
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
	 */
	struct PassDesc
	{
		std::string name;
		Anchor anchor = Anchor::Count;
		std::int32_t order = 0;
		PassKind kind = PassKind::Compute;
		PassQueue queue = PassQueue::Default;
		std::function<std::shared_ptr<org::RenderPass>()> make;
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
	 *   anchor submits nothing. Null: always active. An inactive client's passes still run when another client keeps the epoch:
	 *   a pass with nothing to do records nothing (its Prepare returns empty frame data).
	 * - onFailed: the graph failed and stopped. The client is unregistered; it returns to its D3D11 path.
	 */
	struct ClientDesc
	{
		std::string id;
		std::vector<PassDesc> passes;
		std::function<void(org::RenderGraph&)> registerResources;
		std::function<void(const Point&)> commit;
		std::function<bool()> active;
		std::function<void()> onFailed;
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
	 * at their next execution.
	 */
	bool Register(ClientDesc a_client);

	/** @brief Removes a client; its anchors' graphs are built again at their next execution. Nothing when the id is unknown. */
	void Unregister(std::string_view a_id);

	/**
	 * @brief The client's anchors' graphs built again at their next execution, with its passes and resources as they are then: after
	 * the client replaced a resource it registers (a resize, GraphResources). Render thread. Nothing when the id is unknown.
	 */
	void Refresh(std::string_view a_id);

	/**
	 * @brief The hook: runs the anchor's epoch at this point of the D3D11 stream when it has an active client. After the late
	 * anchors (PostProcessing, Tonemap, Present) it separates the epoch from external Vulkan work that may follow
	 * (Upscaling::SeparateFromGraphEpochs; render-graph.md, "Hazard: external Vulkan work directly after a graph epoch").
	 * @return Whether an epoch was submitted.
	 */
	bool Run(const Point& a_point);

	/** @brief Run for a hook without engine targets. */
	inline bool Run(Anchor a_anchor) { return Run(Point{ .anchor = a_anchor }); }

	/**
	 * @brief The engine's render target (RE::RENDER_TARGET) as a graph texture, imported once and kept while the engine's texture
	 * stays the same. Its views follow the engine's (SRV, RTV, UAV where the engine made one). Registered with every graph as
	 * "cs.engine.target.<index>". Render thread (it synchronizes with DXVK's worker the first time). Null when the target does not
	 * exist or cannot be imported. A separate graph resource from Drawcall Limit Fix's import of the same image, which is safe because
	 * no anchor epoch is one of DCLF's.
	 */
	std::shared_ptr<org::ExternalTextureResource> EngineTarget(std::uint32_t a_target);

	/** @brief EngineTarget for a depth-stencil target (RE::RENDER_TARGETS_DEPTHSTENCIL), with its DSV and depth SRV: "cs.engine.depth.<index>". */
	std::shared_ptr<org::ExternalTextureResource> EngineDepth(std::uint32_t a_depthStencil);

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
	 * reservations; CS_ORG_ANCHOR_SELFTEST=1 reserves every anchor then.
	 */
	void AddToFrame(std::vector<std::uint32_t>& a_epochs);

	/** @brief Initialize, once, with the host made and no graph built: adds the anchors' extension when any anchor has an epoch. */
	void Attach(org::GraphHost& a_host);
}

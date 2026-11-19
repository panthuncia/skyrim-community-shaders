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
* @brief Frame anchors: named points in CS's frame where features attach render-graph passes.
*
* Each anchor is an existing CS hook point. A feature reserves anchors before the graph initializes, then registers a client
* whose passes each name an anchor and an order key. At the hook, Run executes that anchor's passes in order.
* An anchor nobody reserved costs nothing. One with no active client submits nothing.
* Everything here runs on the render owner thread. Pass Prepare and Record run on the graph host's thread, so they read only
* what they were made with or what the commit wrote into a LatchBlock.
* The header stays free of ORG and Vulkan includes.
*/
namespace FrameAnchors
{
	/**
	* @brief A graph texture as passes declare it. Either a slot, whose new same-shape D3D11 textures become versions, or a plain resource.
	* Declare the Texture itself, never Current(). Two handles are equal when they name the same slot, whatever version is current.
	*/
	class Texture
	{
	public:
		Texture() = default;
		Texture(std::nullptr_t) {}

		/** @brief A plain resource, declared as itself. */
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

		/** @brief What stays the same across versions, for a pass's Revision. */
		const void* Key() const { return slot ? static_cast<const void*>(slot.get()) : static_cast<const void*>(plain.get()); }

		/** @brief The import's graph id ("cs.engine.target.1", "Engine::kMAIN"); empty for a plain resource. */
		const std::string& Id() const { return id; }

		/** @brief The current backing, for shape queries and raw handles only. Never declare it or keep it across frames. */
		std::shared_ptr<org::GloballyIndexedResource> Current() const;

		/** @brief Current(), for one call (`texture->GetAPIResource()`). */
		org::GloballyIndexedResource* operator->() const;

		/** @brief The current version's size and format. 0 or Unknown when there is none. */
		std::uint32_t Width() const;
		std::uint32_t Height() const;
		rhi::Format Format() const;

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
		// The engine's own clears and copies on the graph. They run zero or more times a frame, so they are listed last.
		EngineShadowClear,  // a shadow map slice's clear (Renderer::ApplyState in BSShadowLight::RenderShadowmap)
		EngineCubeClear,    // a reflection face's clears (Renderer::ApplyState in BSCubeMapCamera::RenderFaces)
		EngineFrameClear,   // every other engine clear (ApplyState, SetDirtyStates, Renderer::ClearColor and ClearDepthStencil)
		EngineDepthCopy,    // the main depth copied into kPOST_ZPREPASS_COPY (Main::RenderDepth, FinishAccumulating, DeferredPasses)
		// Appended out of frame order so the values above stay. Each Scaleform display's EndDisplay, several times a frame.
		Interface,
		// Appended. The engine's ImageSpace draws the graph makes in the engine's place, a batch at a time.
		ImageSpace,
		// The engine draw families DCLF draws at their own calls, such as shadow masks, water, precipitation and screen blood.
		// Several runs a frame, so it is listed out of frame order.
		EngineDraws,
		// The sun occlusion queries the graph answers, up to three a frame. A client of its own, so every run's queries are that client's.
		EngineQueries,
		// Appended. Volumetric Lighting's GPU work, once a frame where the engine's VL blur would dispatch.
		VolumetricLighting,
		// Appended. The decal occlusion tests at the frame's start (SE 35563, AE 36562). Not EngineQueries, because an inactive
		// client's passes run in other clients' runs, and their query reset and resolve would overwrite unread counts.
		DecalQueries,
		// Appended, native mode only. The engine's copy of kMAIN into "kMAIN Copy", once or twice a frame.
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

	/** @brief The queue a pass prefers. By default compute and copy run on graphics, beside the D3D11 work around them. */
	enum class PassQueue : std::uint8_t
	{
		Default,
		Compute,  // the graph's async compute queue: the anchor's later graphics work waits for it
	};

	/** @brief Where Run was called from. The anchor, and the hook's engine targets where it has them, else -1. */
	struct Point
	{
		Anchor anchor = Anchor::Count;
		std::int32_t input = -1;
		std::int32_t output = -1;
	};

	/**
	* @brief One pass of a client. order only decides between passes with no resource dependency.
	* A once pass stays until one run with it submits, so its work must be safe to run twice. atExecution records it after the commit.
	*/
	struct PassDesc
	{
		std::string name;
		Anchor anchor = Anchor::Count;
		std::int32_t order = 0;
		PassKind kind = PassKind::Compute;
		PassQueue queue = PassQueue::Default;
		std::function<std::shared_ptr<org::RenderPass>()> make;
		// Replaces make for a client with instances. A declared segment makes one pass per instance, an anchor's own epoch instance 0 only.
		std::function<std::shared_ptr<org::RenderPass>(std::uint32_t a_instance)> makeInstance;
		bool once = false;
		bool atExecution = false;
		// With atExecution, every draw and dispatch takes its counts from the run gate. With GPU gates on it records ahead
		// and does nothing on a run whose entries are closed.
		bool gpuGated = false;
		// The pass's bit in its segment's declared rows. With the bit off it records only its barriers. -1 for no row.
		std::int16_t row = -1;
	};

	/**
	* @brief A feature's passes and hooks. Callbacks run on the render thread. Registering an id again replaces it.
	* Inactive clients' passes still run in other clients' runs, so gate per-run work. Only executed confirms a submission.
	*/
	struct ClientDesc
	{
		std::string id;
		std::vector<PassDesc> passes;
		std::function<void(org::RenderGraph&)> registerResources;
		std::function<void(const Point&)> commit;
		std::function<bool()> active;
		bool sharedConstants = false;
		std::vector<std::shared_ptr<RenderGraph::RunGate>> gates;
		std::function<void(const Point&)> executed;
		// Whether the client works this frame, read at BeginFrame from settings and scene state, never from this frame's hooks.
		// Off means its passes record only barriers and its commit is skipped. Null means always on.
		std::function<bool()> declared;
		// For hooks that repeat inside one segment run. The k-th tap of the step this frame is instance k.
		// maxInstances is what passes are built for, instances is read at frame start. Taps past the count are errors.
		std::uint32_t maxInstances = 1;
		std::function<std::uint32_t()> instances;
		// The rows of the client's passes that work this frame, read at frame start only. Null means every row.
		std::function<void(std::array<std::uint64_t, 4>&)> rows;
		// Allows the commit at several taps of one segment run, each writing entries no earlier tap wrote.
		bool repeats = false;
		// Passes only in the declared segments that hold it. Work outside an open window is a tap error and draws nothing.
		bool segmentOnly = false;
		// Make passes instance by instance, so the taps' order holds across targets. Scaleform displays draw over earlier ones.
		bool instanceMajor = false;
		// A registration the schedule does not hold yet taps as the one it holds. Only for clients whose commit every registration reads.
		bool matchesById = false;
		// The client's own count, returned by HeldNow: which registration the frame's schedule holds.
		std::uint64_t generation = 0;
	};

	/** @brief An anchor's name, as the logs and docs give it ("DeferredPasses"). */
	const char* Name(Anchor a_anchor);

	/** @brief Reserves an anchor's epoch. Call before the render graph initializes. Returns false afterwards. */
	bool Reserve(Anchor a_anchor);

	/** @brief Whether the anchor has an epoch (reserved before initialization, and the runtime is up). */
	bool HasEpoch(Anchor a_anchor);

	/** @brief Whether atExecution passes at the anchor record after the run's commit, so they can read it. Render thread, at a build. */
	bool RecordsAtExecution(Anchor a_anchor);

	/** @brief The Revision for a pass whose Record reads its run's slot.
	*  Recorded at execution it never changes. Recorded ahead, every call is new, so the ticket is prepared again after the commit. */
	inline void CommitReadRevision(bool a_atExecution, std::vector<std::uint64_t>& a_out)
	{
		static std::atomic<std::uint64_t> ahead{ 0 };
		a_out.push_back(a_atExecution ? 0 : ahead.fetch_add(1, std::memory_order_relaxed) + 1);
	}

	/** @brief Registers or replaces a client. Render thread. Returns false, logged, on a bad desc or an inactive runtime.
	*  A client reading the shared constants before CS's first upload waits until they exist. */
	bool Register(ClientDesc a_client);

	/** @brief Removes a client. Until its anchors rebuild, old graphs still hold its passes, with gates closed and no callbacks. */
	void Unregister(std::string_view a_id);

	/** @brief Rebuilds the client's anchors after it replaced a resource outside GraphResources. Render thread. */
	void Refresh(std::string_view a_id);

	/** @brief Whether the anchor's last built graph holds this registration of a_id. Render thread. */
	bool InBuiltGraph(std::string_view a_id, Anchor a_anchor);

	/** @brief Whether a run of a_anchor now would draw a_id's work. Ask before taking the engine's work. Render thread.
	*  Run can still fail afterwards. The executed callback confirms submission. */
	bool Scheduled(std::string_view a_id, Anchor a_anchor);

	/**
	* @brief Whether this frame's schedule draws a_id's work at a_anchor, from the schedules the stages took at Renderer::Begin.
	* True when every stage of this frame with a step at a_anchor holding a_id has this registration on, and one such stage exists.
	* Unlike Scheduled it does not depend on which stage is open, so it gives one answer for the whole frame. Render owner.
	*/
	bool ScheduledThisFrame(std::string_view a_id, Anchor a_anchor);

	/**
	* @brief Whether every stage of this frame with a step at a_anchor holding a_id has ended this frame, and one such stage exists.
	* Then a tap of a_anchor now comes after those stages, such as a menu scene's tonemap after the post stage, and no stage takes it.
	* Render owner.
	*/
	bool StagePassed(std::string_view a_id, Anchor a_anchor);

	/** @brief Whether the stage holding a_anchor wholly has a_id's current registration in its last schedule.
	*  For work queued before the frame's Begin, such as a Bink tick. Render owner. */
	bool InStage(std::string_view a_id, Anchor a_anchor);

	/** @brief HeldNow's answer. */
	struct StageHold
	{
		bool held = false; // a run of the anchor now taps a CS stage
		bool scheduled = false;  // the frame's schedule of that stage holds a registration of the client (by id) and has it on
		std::uint64_t generation = 0;  // that registration's ClientDesc::generation
	};

	/** @brief Whether a run of a_anchor now taps a stage holding a_id, and which registration its schedule holds.
	*  Held and scheduled draws. Held and not scheduled lags. Not held has no place now. Render owner. */
	StageHold HeldNow(std::string_view a_id, Anchor a_anchor);

	/** @brief The client's one-shot passes (PassDesc::once) pending again: each runs at its anchor's next run. Render thread. */
	void Rearm(std::string_view a_id);

	/** @brief The hook run after each late anchor's epoch, since an upscaler, frame generation or the present may follow.
	*  One hook. Installing another replaces it. */
	void SetLateEpochHook(std::function<void()> a_hook);

	/** @brief What a run did. */
	enum class RunStatus : std::uint8_t
	{
		Ran,       // the epoch was submitted; the executed callbacks of the clients in the built graph ran
		Idle,      // nothing to run: no client active and no one-shot pass pending, or the anchor has no epoch
		Refused,   // the caller does not own rendering (IsRenderOwner), or the runtime is not active
		Failed,    // the epoch failed (its clients are dropped at the next poll), or the graph stopped
	};

	/** @brief Run's result. True only for Ran. For any other status, a caller that withheld the engine's work draws it through the engine. */
	struct RunResult
	{
		RunStatus status = RunStatus::Idle;
		constexpr operator bool() const noexcept { return status == RunStatus::Ran; }
	};

	/** @brief Runs the anchor's epoch here when it has an active client or a pending one-shot pass. Render owner only.
	*  Each run takes its own run lease, so it never waits for another run of the same frame. */
	RunResult Run(const Point& a_point);

	/** @brief Run for a hook without engine targets. */
	inline RunResult Run(Anchor a_anchor) { return Run(Point{ .anchor = a_anchor }); }

	/** @brief Whether the calling thread is inside Run. A run never starts another. Any thread. */
	bool InRun();

	/** @brief Whether the calling thread owns rendering now, the only thread where anchors run. A pure query. Any thread.
	*  With CS_ORG_LOADING_THREAD=1 it is the thread that last began an engine frame. A false answer is always safe. */
	bool IsRenderOwner();

	/** @brief Makes the calling thread the render owner, at each engine frame start (Renderer::Begin, SE 75460, AE 77245).
	*  Only with CS_ORG_LOADING_THREAD=1. */
	void ClaimRenderOwnership();

	/** @brief How many times rendering changed threads so far (CS_ORG_LOADING_THREAD=1). Any thread. */
	std::uint64_t RenderOwnerChanges();

	/** @brief Records the calling thread as the engine's main thread. SKSE loads plugins there. */
	void LatchMainThread();

	/**
	* @brief An engine render target as a graph slot, registered as "cs.engine.target.<index>". Null if it cannot be imported.
	* Render thread. Each image has one graph resource, so this may return an existing slot or ORG's own resource instead.
	*/
	Texture EngineTarget(std::uint32_t a_target);

	/** @brief EngineTarget for a depth-stencil target (RE::RENDER_TARGETS_DEPTHSTENCIL), with its DSV and depth SRV: "cs.engine.depth.<index>". */
	Texture EngineDepth(std::uint32_t a_depthStencil);

	/** @brief EngineTarget for a cubemap target, with cube views and its faces' RTVs: "cs.engine.cubemap.<index>". */
	Texture EngineCubemap(std::uint32_t a_cubemap);

	/** @brief Imports a texture a feature still owns in D3D11 as "cs.d3d11.<name>". Any view may be null.
	*  The first import of a name decides its views, so import it the same way everywhere. */
	Texture D3D11Texture(std::string_view a_name, ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_srv,
		ID3D11UnorderedAccessView* a_uav, ID3D11RenderTargetView* a_rtv = nullptr);

	/** @brief Imports a texture loaded once from a file and only read after, such as a noise or LUT texture.
	*  Native mode makes it one untracked graph resource that passes may only read. Elsewhere it imports as D3D11Texture does. */
	Texture ImmutableTexture(std::string_view a_name, ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_srv);

	/** @brief EngineDepth for a depth texture outside the engine's table. Registered as a_id and kept while the texture stays the same. */
	Texture ImportDepth(const RE::BSGraphics::DepthStencilData& a_target, const std::string& a_id);

	/** @brief Imports any other D3D11 texture with the views a_views names, registered as a_id. For targets like Scaleform's and ImGui's. */
	Texture ImportTexture(std::string_view a_id, ID3D11Texture2D* a_texture, const org::TextureDescription& a_views);

	/**
	* @brief The one graph resource of an image used by a graph outside the anchors, such as DCLF's epochs. Render thread.
	* The slot keeps a_texture alive so the handle is never reused. A later id import of the same image takes the slot over.
	*/
	Texture ShareImage(ID3D11Texture2D* a_texture, const DxvkOrgInteropImageInfo& a_image, const org::TextureDescription& a_desc, std::string_view a_name);

	/** @brief Whether a CS stage holds uncommitted staged values. D3D11 work reaching DXVK now runs ahead of it. Any thread, no lock. */
	bool StagesHoldValues();

	/** @brief The CS stages holding staged values now, a bit per SegmentId, for the residue census. Any thread, no lock. */
	std::uint32_t StagesHoldingValues();

	/** @brief The hook called when the last open stage window closes, to apply what was kept from DXVK inside the windows. One hook. */
	void SetWindowsClosedHook(void (*a_hook)());

	/** @brief Render owner. Whether the description being read is a stage's build key rather than a frame's.
	*  A rows callback then sets every row a frame may pick, plus requested and always-present rows. */
	bool DescribingKey();

	/** @brief Inside a rows callback, whether the build the frame is described against has a_row present. Empty when unknown. Render owner. */
	std::optional<bool> DescribedBuildHasRow(std::uint32_t a_row);

	/** @brief Whether the calling thread is the engine's main thread (LatchMainThread). Any thread. */
	bool IsMainThread();

	/** @brief Inside a commit at a stage's tap, the instance it writes, counted from 0 per step. 0 outside a segment commit. Render owner. */
	std::uint32_t TapInstance();

	/** @brief Whether a tap of a_id now would commit into an open declared segment whose built graph holds this registration. Render owner. */
	bool CanTap(std::string_view a_id);

	/** @brief Whether a_id's work that the schedule leaves out may lag rather than never draw. Render owner.
	*  False when the stage runs its current description on a build without the client and no such build is coming. */
	bool TapMayLag(std::string_view a_id);

	/** @brief Whether a CS stage holds a_anchor wholly, so it has no epoch of its own. Fixed after init. Any thread. */
	bool HeldByStage(Anchor a_anchor);

	/** @brief Whether CS stage a_segment is open now. Native mode, render owner. False otherwise. */
	bool StageOpen(std::uint32_t a_segment);

	/** @brief Whether CS stage a_segment is declared but has no schedule yet, so its work lags. Native mode, render owner. */
	bool StageAbsent(std::uint32_t a_segment);

	/** @brief Drops the kept recordings of stages holding a_anchor, for work that ends, such as a Bink movie. Render owner.
	*  A late batch keeps what its last recording retained until it records again. a_why is logged. */
	void DropKeptRecordings(Anchor a_anchor, std::string_view a_why);

	/** @brief For a client with instances at a wholly held anchor, the instance whose passes come first in the stage's graph.
	*  Empty without stages. Any thread, after init. */
	std::optional<std::uint32_t> FirstGraphInstance(Anchor a_anchor);

	/** @brief Whether a CS stage names client a_id at one of its steps. Fixed after init. Any thread. */
	bool NamedByStage(std::string_view a_id);

	/** @brief Whether a CS stage has a step at a_anchor. Works before reservation, so modules reserve only where a stage uses an anchor. */
	bool StageStepsAt(Anchor a_anchor);

	/** @brief Writes client a_id's frame inputs outside its tap, while its stage window is open and the schedule has it on.
	*  For data the engine hands over at another call, such as the clear colour. Returns false otherwise. Render owner. */
	bool StageInputs(std::string_view a_id, const std::function<void()>& a_write);

	/** @brief Which of a_ids an open stage gives work handed over outside a tap, such as a clear. -1 if none. Render owner. */
	std::int32_t StagePlace(std::span<const std::string_view> a_ids);

	/** @brief A CS point that ends stage a_segment and is no anchor. Does nothing outside the stage's window. Render owner. */
	RunResult TapPoint(RenderGraph::Frame::SegmentId a_segment);

	/** @brief The gate entry (or latch region index) of a_entry for instance a_instance, a_perInstance entries an instance. */
	constexpr std::uint32_t InstanceEntry(std::uint32_t a_instance, std::uint32_t a_perInstance, std::uint32_t a_entry)
	{
		return a_instance * a_perInstance + a_entry;
	}

	/** @brief The segment whose epoch an anchor's runs submit, its own or its stage's. Any thread, after Initialize. */
	std::uint32_t RunSegment(Anchor a_anchor);

	/** @brief At Renderer::Begin, each declared segment takes its slot's schedule for the frame. Nothing waits.
	*  A segment with no schedule on the attached build is absent, with graph 0. */
	void BeginFrame(RenderGraph::Frame::Description& a_frame);

	/** @brief The declared segments' epochs. Empty with segments off. */
	std::vector<std::uint32_t> SegmentEpochs();

	/** @brief SegmentEpochs with each epoch's segment (a RenderGraph::Frame::SegmentId value), for the frame arena's spans. */
	std::vector<std::pair<std::uint8_t, std::uint32_t>> SegmentEpochsById();

	/** @brief Hands another module's epoch passes to the declared segment that holds it. Render thread.
	*  a_passes is a std::vector<org::RenderGraph::ExternalPassDesc>*. a_declared says at frame start whether they work. */
	void SetForeignPasses(std::uint32_t a_segment, std::function<void(std::uint32_t a_epoch, void* a_passes)> a_gather, std::function<bool()> a_declared);

	/** @brief Whether a declared segment holds module epoch a_segment, so the module gathers nothing itself. Any thread, after Initialize. */
	bool HoldsForeign(std::uint32_t a_segment);

	/** @brief The module epoch's hook when a stage holds it. a_commit writes the run's values and the stage submits them. Render owner. */
	RunResult TapForeign(std::uint32_t a_segment, const std::function<void()>& a_commit);

	/** @brief GPU work is about to be submitted. Stages whose commit point this is commit first. Render owner. */
	void BeforeGpuWork(const char* a_why, std::uint32_t a_segment = UINT32_MAX);

	// What RenderGraphRuntime asks of the anchors.

	/** @brief The anchor's segment (RenderGraphRuntime::Segment, as its value). */
	std::uint32_t SegmentOf(Anchor a_anchor);

	/** @brief Whether a segment value is an anchor's that has no epoch (no reservation): the runtime leaves it out everywhere. */
	bool IsUnusedSegment(std::uint32_t a_segment);

	/** @brief An anchor segment's names for events, logs, the profiler and the contributor ABI. */
	struct SegmentNames
	{
		const char* label;
		const char* inputs;
		const char* profiler;
		const char* contributor;
	};
	const SegmentNames* NamesOf(std::uint32_t a_segment);

	/** @brief Setup's build point. Builds the graphs waiting clients need before the first frame. Returns whether all joined.
	*  After the first world frame builds go to the build manager, and this returns false. */
	bool BuildPending(std::string_view a_caller);

	/** @brief Before the first frame, records the UI stage for every slot so the first frame draws it. Setup waits, no frame does. */
	void PrimeStages(std::string_view a_caller);

	/** @brief Logs each reserved anchor's peak runs a frame (CS_ORG_EPOCH_STATS's report). Render thread. */
	void LogRunStats();

	/** @brief Inserts the reserved anchors' epochs into a_epochs at their place in the frame. The first call freezes reservations. */
	void AddToFrame(std::vector<std::uint32_t>& a_epochs);

	/** @brief Keeps several tickets prepared per anchor epoch, so back-to-back runs find them ready. CS_ORG_TICKETS_AHEAD caps it.
	*  Safe because anchor passes read only what they were made with or what a commit wrote into a LatchBlock. */
	void SetTicketsAhead(org::GraphHost& a_host);

	/** @brief An anchor's epoch failed to build or prepare. Its clients fail, since a graph mode has no D3D11 path. Render thread.
	*  Development builds stop. Release builds unregister them and rebuild their anchors. */
	void OnEpochFailed(std::uint32_t a_epoch, const std::string& a_error, bool a_keptGraph);

	/** @brief Initialize, once, with the host made and no graph built: adds the anchors' extension when any anchor has an epoch. */
	void Attach(org::GraphHost& a_host);
}

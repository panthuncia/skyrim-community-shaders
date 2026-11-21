#pragma once

#include "RenderGraph/Engine/Targets/TargetRef.h"
#include "RenderGraph/Frame/Placement.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
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
	struct FramePreparationContext;
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
* Each anchor is an existing CS hook point, and every anchor is a step of a CS stage (Frame/RecipeTable.h). A feature registers a
* client whose passes each name an anchor and an order key. At the hook, Run taps the stage whose step takes the anchor now.
* An anchor with no active client submits nothing. Anchors have no epochs of their own; the stages do.
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

		/**
		* @brief The current (adopted) version's size and format. 0 or Unknown when there is none. For the render thread, such as a
		* commit. A pass's make or Prepare may run for a revision that is not adopted yet, so it reads PreparedSize instead.
		*/
		std::uint32_t Width() const;
		std::uint32_t Height() const;
		rhi::Format Format() const;

		/** @brief A width and a height. */
		struct Size
		{
			std::uint32_t width = 0, height = 0;
		};

		/**
		* @brief In a pass's Prepare: the size of the version a_context's preparation binds. That is the version the revision being
		* prepared names, or the current one when the revision does not change the slot, as ORG binds it. A plain resource's own
		* size. No version, or a version with no size, is fatal.
		*/
		Size PreparedSize(const org::FramePreparationContext& a_context) const;

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
	* atExecution records it after the commit. Work that runs once is a first-run gate (RunGate.h), never a pass that leaves the graph.
	*/
	struct PassDesc
	{
		std::string name;
		Anchor anchor = Anchor::Count;
		std::int32_t order = 0;
		PassKind kind = PassKind::Compute;
		PassQueue queue = PassQueue::Default;
		std::function<std::shared_ptr<org::RenderPass>()> make;
		// Replaces make for a client with instances. A stage makes one pass per instance.
		std::function<std::shared_ptr<org::RenderPass>(std::uint32_t a_instance)> makeInstance;
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
		// Which registration this is. DeclareClient with a state sets it; leave it 0 elsewhere. A client with a generation taps by id:
		// a run commits the newest registration's callbacks for the one the schedule holds, and the commit finds that one's state
		// with DeclaredClientOf::Held (Placement::generation).
		std::uint64_t generation = 0;
	};

	/** @brief An anchor's name, as the logs and docs give it ("DeferredPasses"). */
	const char* Name(Anchor a_anchor);

	/**
	* @brief Whether atExecution passes at a shared anchor record after the run's commit, so they can read it, as its stages' epochs
	* record. False for an anchor a stage holds wholly, whose passes record ahead. Render thread, at a build.
	*/
	bool RecordsAtExecution(Anchor a_anchor);

	/** @brief The ticket's revision value for a pass whose Record reads its run's slot (not a frame revision).
	*  Recorded at execution it never changes. Recorded ahead, every call is new, so the ticket is prepared again after the commit. */
	inline void CommitReadTicket(bool a_atExecution, std::vector<std::uint64_t>& a_out)
	{
		static std::atomic<std::uint64_t> ticketSerial{ 0 };
		a_out.push_back(a_atExecution ? 0 : ticketSerial.fetch_add(1, std::memory_order_relaxed) + 1);
	}

	// Register is private to Declare (Frame/DeclareRegistration.h): features register through Frame::DeclareClient.

	/** @brief Removes a client. Until its anchors rebuild, old graphs still hold its passes, with gates closed and no callbacks. */
	void Unregister(std::string_view a_id, const std::source_location& a_where = std::source_location::current());

	/**
	* @brief Where a run of a_anchor now puts client a_id's work. Render owner. The one frame query for a client.
	* - At an anchor a stage holds wholly, that stage. At a shared anchor, the open stage whose step takes the anchor's taps now and holds
	*   the client.
	* - With a_anchor Count, any open stage that holds the client, for work handed over outside a tap.
	* A registration matches as the run's commit does: by id for a client with a generation (DeclareClient with a state), else exactly.
	* Run can still fail after a scheduled answer. The executed callback confirms submission.
	*/
	Placement Place(std::string_view a_id, Anchor a_anchor = Anchor::Count);

	/** @brief The hook run after each late anchor's tap, since an upscaler, frame generation or the present may follow.
	*  One hook. Installing another replaces it. */
	void SetLateEpochHook(std::function<void()> a_hook);

	/** @brief What a run did. */
	enum class RunStatus : std::uint8_t
	{
		Ran,       // the epoch was submitted; the executed callbacks of the clients in the built graph ran
		Idle,      // nothing to run: no client active, or no stage takes the tap now
		Refused,   // the caller does not own rendering (IsRenderOwner), or the runtime is not active
		Failed,    // the epoch failed (its clients are dropped at the next poll), or the graph stopped
	};

	/** @brief Run's result. True only for Ran. For any other status, a caller that withheld the engine's work draws it through the engine. */
	struct RunResult
	{
		RunStatus status = RunStatus::Idle;
		constexpr operator bool() const noexcept { return status == RunStatus::Ran; }
	};

	/** @brief Taps the stage whose step takes the anchor now, when it has an active client. Render owner only.
	*  Each run takes its own run lease, so it never waits for another run of the same frame. */
	RunResult Run(const Point& a_point);

	/** @brief Run for a hook without engine targets. */
	inline RunResult Run(Anchor a_anchor) { return Run(Point{ .anchor = a_anchor }); }

	/** @brief Whether the calling thread is inside Run. A run never starts another. Any thread. */
	bool InRun();

	/** @brief Whether the calling thread owns rendering now, the only thread where anchors run. A pure query. Any thread.
	*  In the graph modes it is the thread that last began an engine frame. A false answer is always safe. Revisions adopt only
	*  on the main thread (IsMainThread), whichever thread owns rendering. */
	bool IsRenderOwner();

	/** @brief Makes the calling thread the render owner, at each engine frame start (Renderer::Begin, SE 75460, AE 77245).
	*  Only in graph mode (Engine::GraphMode). */
	void ClaimRenderOwnership();

	/** @brief How many times rendering changed threads so far, in the graph modes. Any thread. */
	std::uint64_t RenderOwnerChanges();

	/** @brief Records the calling thread as the engine's main thread. SKSE loads plugins there. */
	void LatchMainThread();

	/**
	* @brief How imports change (frame revision plan, step 9b). The first import of an id makes its slot, with every view the texture's
	* bind flags allow in its format plus the views the caller names. Later calls with the same texture return the slot. A new texture
	* under the id, or a view the slot lacks, is a change: inside the frame's adoption point (a declared client's key, make or adopted
	* call) it becomes a pending version of the revision being prepared, adopted with it. Anywhere else it is TapError::OutsideAdoption,
	* naming the id and a_caller, and the slot keeps its version. So import what can change in a declared client's key, never mid-frame.
	*/

	// The three import entry points: EngineTexture for the engine's targets, ImportTexture for any other D3D11 texture, and
	// ShareImage for an image a graph outside the anchors uses.

	/** @brief Names of the engine's targets for EngineTexture: FrameAnchors::Colour(RE::RENDER_TARGET::kMAIN) and the like. */
	using RenderGraph::Engine::Targets::Colour;
	using RenderGraph::Engine::Targets::ColourCopy;
	using RenderGraph::Engine::Targets::Cubemap;
	using RenderGraph::Engine::Targets::Depth;
	using RenderGraph::Engine::Targets::TargetRef;

	/**
	* @brief An engine target as a graph slot: the engine target set's declared slot, stable for the session. Render thread.
	* A colour target the engine keeps (a mip-generating one), or any colour target before the set is declared, is imported as
	* "cs.engine.target.<index>"; each image has one graph resource, so that may return an existing slot. Other kinds are null
	* before the set is declared. Null for an index past the engine's table. The view and slice of a_target are ignored.
	*/
	Texture EngineTexture(TargetRef a_target, const std::source_location& a_caller = std::source_location::current());

	/** @brief What ImportTexture imports. Set texture and its views, or depth, or texture and views. */
	struct ImportSource
	{
		ID3D11Texture2D* texture = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;  // any view may be null
		ID3D11UnorderedAccessView* uav = nullptr;
		ID3D11RenderTargetView* rtv = nullptr;
		const RE::BSGraphics::DepthStencilData* depth = nullptr;  // a depth target outside the engine's table: its texture and views
		const org::TextureDescription* views = nullptr;           // exactly these views, for targets like Scaleform's and ImGui's
		// Loaded once from a file and only read after, such as a noise or LUT texture. Native mode makes it one untracked graph
		// resource that passes may only read; elsewhere it imports like any other.
		bool immutable = false;
	};

	/**
	* @brief Imports a D3D11 texture a feature or the engine still owns, registered as a_id. A feature's own texture is named
	* "cs.d3d11.<name>" by convention. Null for an empty id. The first import of an id decides its views, so import it the same way
	* everywhere. A depth source is kept while its texture stays the same.
	*/
	Texture ImportTexture(std::string_view a_id, const ImportSource& a_source, const std::source_location& a_caller = std::source_location::current());

	/**
	* @brief The one graph resource of an image used by a graph outside the anchors, such as DCLF's epochs. Render thread.
	* The slot keeps a_texture alive so the handle is never reused. A later id import of the same image takes the slot over.
	*/
	Texture ShareImage(ID3D11Texture2D* a_texture, const DxvkOrgInteropImageInfo& a_image, const org::TextureDescription& a_desc, std::string_view a_name,
		const std::source_location& a_caller = std::source_location::current());

	/** @brief The CS stages holding staged values now, a bit per SegmentId, for the residue census. Any thread, no lock. */
	std::uint32_t StagesHoldingValues();

	/** @brief Render owner. Whether the description being read is a stage's build key rather than a frame's.
	*  A rows callback then sets every row a frame may pick, plus requested and always-present rows. */
	bool DescribingKey();

	/** @brief Inside a rows callback, whether the build the frame is described against has a_row present. Empty when unknown. Render owner. */
	std::optional<bool> DescribedBuildHasRow(std::uint32_t a_row);

	/** @brief Whether the calling thread is the engine's main thread (LatchMainThread). Any thread. */
	bool IsMainThread();

	/** @brief Inside a commit at a stage's tap, the instance it writes, counted from 0 per step. 0 outside a segment commit. Render owner. */
	std::uint32_t TapInstance();

	/** @brief Whether a CS stage holds a_anchor wholly, rather than sharing it with other stages. False before init. Any thread. */
	bool HeldByStage(Anchor a_anchor);

	/**
	* @brief CS stage a_segment's state now. Native mode, render owner. Reason None while its window is open, Absent while it is
	* declared in this frame with no schedule yet (its work lags), NoStage otherwise. The stage is set for None and Absent.
	*/
	Placement Stage(std::uint32_t a_segment);

	/** @brief Drops the kept recordings of stages holding a_anchor, for work that ends, such as a Bink movie. Render owner.
	*  A late batch keeps what its last recording retained until it records again. a_why is logged. */
	void DropKeptRecordings(Anchor a_anchor, std::string_view a_why);

	/** @brief For a client with instances at a wholly held anchor, the instance whose passes come first in the stage's graph.
	*  Empty without stages. Any thread, after init. */
	std::optional<std::uint32_t> FirstGraphInstance(Anchor a_anchor);

	/** @brief Whether a CS stage names client a_id at one of its steps. Fixed after init. Any thread. */
	bool NamedByStage(std::string_view a_id);

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

	/** @brief At Renderer::Begin, each declared segment takes its slot's schedule for the frame. Nothing waits.
	*  A segment with no schedule on the attached build is absent, with graph 0. */
	void BeginFrame(RenderGraph::Frame::Description& a_frame);

	/** @brief The CS stages' epochs. Empty before Initialize. */
	std::vector<std::uint32_t> SegmentEpochs();

	/** @brief GPU work is about to be submitted. Stages whose commit point this is commit first. Render owner. */
	void BeforeGpuWork(const char* a_why, std::uint32_t a_segment = UINT32_MAX);

	// What RenderGraphRuntime asks of the anchors.

	/** @brief Whether a segment value is a CS stage's that is not in the frame: the runtime leaves it out everywhere. */
	bool IsUnusedSegment(std::uint32_t a_segment);

	/** @brief A CS stage segment's names for events, logs and the profiler. */
	struct SegmentNames
	{
		const char* label;
		const char* inputs;
		const char* profiler;
	};
	const SegmentNames* NamesOf(std::uint32_t a_segment);

	/** @brief Setup's build point. Builds the graphs waiting clients need before the first frame. Returns whether all joined.
	*  After the first world frame builds go to the build manager, and this returns false. */
	bool BuildPending(std::string_view a_caller);

	/** @brief Before the first frame, asks for the UI stage's recording for every slot. Nothing waits; each slot switches it in once recorded. */
	void PrimeStages(std::string_view a_caller);

	/** @brief Inserts the CS stages' epochs into a_epochs at their place in the frame. The first call fixes the stages for the session. */
	void AddToFrame(std::vector<std::uint32_t>& a_epochs);

	/** @brief A stage's epoch failed to build or prepare. Its clients fail, since a graph mode has no D3D11 path. Render thread.
	*  Development builds stop. Release builds unregister them and rebuild their stages. */
	void OnEpochFailed(std::uint32_t a_epoch, const std::string& a_error, bool a_keptGraph);

	/** @brief Initialize, once, with the host made and no graph built: adds the anchors' extension. */
	void Attach(org::GraphHost& a_host);
}

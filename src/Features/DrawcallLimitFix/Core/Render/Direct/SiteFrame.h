#pragma once

// The direct sites' shared internals: frame slots, cull dispatch, the callback's recording helpers and each site file's hooks.
// Sites.cpp owns the frame and the main depth, colour and forward sites. ShadowSite.cpp, ViewSites.cpp and Families.cpp hold the rest.
// Included by those files only. Include Main/Resources.h first so volk comes before any Vulkan header.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Recorder.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"

#include <rhi.h>

namespace org
{
	class Buffer;
	class ExternalEpochExecution;
}

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <ORGModuleServices/RecordingPools.h>
#include <ORGModuleServices/SecondaryListPool.h>
#include <Render/RunRef.h>

namespace org
{
	class GloballyIndexedResource;
}

namespace DCLF::Direct::detail
{
	using Clock = std::chrono::steady_clock;
	inline constexpr std::size_t kSites = static_cast<std::size_t>(Site::kCount);

	inline double MicrosecondsSince(Clock::time_point a_start)
	{
		return std::chrono::duration<double, std::micro>(Clock::now() - a_start).count();
	}

	template <class T>
	void Add(T& a_counter, T a_value)
	{
		std::atomic_ref<T>(a_counter).fetch_add(a_value, std::memory_order_relaxed);
	}

	/** @brief DXVK's CS thread's secondaries for one frame slot, from their own command pool (OMS). Reset when the slot retires. */
	using ListPool = org::services::SecondaryListPool;

	/**
	 * @brief A frame slot's CPU recording memory: a cull arena per pool worker and DXVK's CS thread's secondaries. Retired when the
	 * slot is released, through ListPools.
	 */
	using SlotRecording = org::services::RecordingSlot<ListPool>;

	/** @brief List pools reset on DCLF's pool, since each reset is a driver call. Any thread hands in, the render thread takes. */
	org::services::ResetPools<ListPool>& ListPools();

	/** @brief DCLF's own views of the main targets, made once per resources. */
	struct TargetViews
	{
		// Hold the images as long as the views and key on them. A Resources address can be reused, and views of a released image
		// once drew into freed memory DXVK had reused.
		std::array<std::shared_ptr<org::GloballyIndexedResource>, Draws::kColorTargets> native{};
		std::shared_ptr<org::GloballyIndexedResource> nativeDepth, refractionNormals;
		rhi::DescriptorHeapPtr rtvHeap, dsvHeap;
		std::uint32_t count = 0;
		std::array<rhi::DescriptorSlot, Draws::kColorTargets> colours{};
		std::array<rhi::ResourceHandle, Draws::kColorTargets> colourResources{};
		std::array<rhi::Format, Draws::kColorTargets> formats{};
		rhi::DescriptorSlot depth{};
		rhi::ResourceHandle depthResource{};  // the image the DSV names, to notice a new image of the same resource
		rhi::Format depthFormat = rhi::Format::Unknown;
		rhi::DescriptorSlot refraction{};
		rhi::ResourceHandle refractionResource{};
		rhi::Format refractionFormat = rhi::Format::Unknown;
		bool valid = false;
		// Views a recording resolved instead (SiteImages) name ORG's slots and keep the views made at commit, with their heaps.
		std::shared_ptr<const void> keep;

		/** @brief These views without their heaps, keeping these alive. For a run that rebinds some of them. */
		static std::shared_ptr<TargetViews> RebindOf(const std::shared_ptr<const TargetViews>& a_views)
		{
			auto views = std::make_shared<TargetViews>();
			views->native = a_views->native;
			views->nativeDepth = a_views->nativeDepth;
			views->refractionNormals = a_views->refractionNormals;
			views->count = a_views->count;
			views->colours = a_views->colours;
			views->colourResources = a_views->colourResources;
			views->formats = a_views->formats;
			views->depth = a_views->depth;
			views->depthResource = a_views->depthResource;
			views->depthFormat = a_views->depthFormat;
			views->refraction = a_views->refraction;
			views->refractionResource = a_views->refractionResource;
			views->refractionFormat = a_views->refractionFormat;
			views->valid = a_views->valid;
			views->keep = a_views;
			return views;
		}
	};

	/** @brief One view of a site in a frame, with its cull inputs and culled batches. */
	struct SiteView
	{
		ViewType type = ViewType::Main;
		CullView view;
		bool depthPass = false;
		// Main colour takes its object words from the depth site's batches. mainView culls them when the depth site did not run.
		bool colourWords = false;
		CullView mainView;
		const std::vector<ViewBatch>* list = nullptr;
		CulledQueue* queue = nullptr;
		std::vector<const CulledBatch*> arrived;  // by batch index
		std::uint32_t received = 0;
		// Null uses the run's bindings.
		const RecordBindings* bindings = nullptr;
		// A CPU frame's forward and refraction views: their draws are sorted on the pool (StageForward) once the culls and the
		// render thread's rows are both in (each arrives at stageGate once), so the recording thread only records them. A recording
		// that comes first takes the gate and sorts them itself.
		bool stageArmed = false;
		const RecordBindings* stageBindings = nullptr;
		std::atomic<std::uint32_t> cullJobs{ 0 };
		std::atomic<int> stageGate{ 0 };
		std::atomic<bool> staged{ false };
		std::vector<StagedForwardDraw> stagedDraws;
	};

	struct FrameSlot;

	/** @brief One site's frame, with its inputs, views and what its callback records. */
	struct SiteRun
	{
		Site site = Site::Depth;
		RecordMode record = RecordMode::Serial;
		FrameSlot* slot = nullptr;
		rhi::Device device;
		std::shared_ptr<const Draws::PassFrame> shape;
		std::shared_ptr<Draws::Resources> resources;
		std::shared_ptr<const TargetViews> targets;  // null when the site attaches its own
		// The epoch's ORG execution, run by the callback. Its waits and signals join DXVK's submission.
		std::shared_ptr<org::ExternalEpochExecution> execution;
		std::shared_ptr<const CullFrame> cullFrame;  // written only by the dispatch
		RecordBindings bindings, refractionBindings;
		std::vector<SiteView*> views;
		SiteView *main = nullptr, *decal = nullptr, *forward = nullptr, *refractionView = nullptr;
		bool refraction = false;
		// A water site's list (the water mask's or surface's), and the target set its trigger chose (DeferredRangeContribution::variant).
		SiteView* water = nullptr;
		std::uint32_t waterVariant = 0;
		// False when the run's inputs were incomplete. Its queues are still drained.
		bool valid = true;
		bool recordedDraws = false;
		// SiteCallback runs once per segment, so remember the count at the first.
		std::uint32_t drawnBefore = 0;
		// Per-file state for this run, released with it when the slot retires.
		std::shared_ptr<void> shadowState, viewState, familyState;
		// The CPU-frame timeline value after which its site stats can be read.
		std::uint64_t frameValue = 0;
	};

	/** @brief Everything a CPU frame's sites made, kept until the frame's signal completes. */
	struct FrameSlot
	{
		// Index into the sites' per-slot tables. Forward slots come after the CPU frames' slots.
		std::uint32_t index = 0;
		std::uint64_t signal = 0;  // last frame's CPU-frame timeline value, 0 for none
		bool used = false;
		// False for a GPU-driven frame's forward family alone. Such a slot has no signal and retires by its point on the retirement clock.
		bool ownsEpochs = true;
		std::uint64_t retirePoint = 0;
		bool retireNoted = false;
		SlotRecording recording;  // the pool workers' cull arenas and DXVK's CS thread's secondaries
		std::vector<std::unique_ptr<CulledQueue>> queues;  // reused, because an EventQueue's ring is never freed
		std::size_t queuesUsed = 0;
		std::deque<SiteView> views;
		std::deque<SiteRun> runs;
		std::vector<std::shared_ptr<const void>> holds;
		std::shared_ptr<const ViewBatchSet> set;
		std::uint32_t sceneFrame = 0;
		// The pool's first indices after the frame's first drawing commit. The recorder adds them per draw.
		std::vector<std::uint32_t> poolFirsts;
		bool poolFirstsTaken = false;
		// Placement rows for the forward and refraction sort on the CS thread. Only those objects are written.
		std::vector<BindlessPlacement> forwardRows;
		// The main list's depth batches, which colour's object words come from, and the gate that dispatches colour culls.
		std::unique_ptr<std::atomic<const CulledBatch*>[]> mainDepth;
		std::size_t mainDepthCount = 0;
		std::atomic<std::uint32_t> mainDepthPending{ 0 };
		std::atomic<int> colourGate{ 0 };
		std::function<void()> colourDispatch;
		bool depthRan = false;
		SiteRun* forwardRun = nullptr;
		// The water mask's and surface's runs, opened by the colour commit and taken at their triggers.
		std::array<SiteRun*, 2> waterRuns{};
		std::shared_ptr<void> shadowState, viewState, familyState;
		// Pool work still running for this slot's frame. A continuation that starts after the slot moves on does nothing.
		// Release waits until none is left.
		std::atomic<std::uint32_t> outstanding{ 0 };
		std::atomic<std::uint64_t> generation{ 0 };
		FrameCounters counters;
		std::array<std::uint32_t, kSites> siteRuns{}, siteEmpty{};
		std::uint32_t waits = 0;
		// Written by cull jobs and read by the GPU until the frame's signal. A frame that needs more grows it, and its overflow draws directly.
		std::shared_ptr<org::Buffer> drawBuffer;
		IndirectBuffer indirect;
		std::atomic<std::uint32_t> drawCursor{ 0 };
	};

	struct State
	{
		rhi::Device device;
		bool deviceSet = false;
		std::shared_ptr<rhi::TimelinePtr> timeline;  // cs.dclf.cpu-frames, on the host's device
		std::uint64_t signalled = 0;  // the timeline's last value queued
		std::uint64_t submitted = 0;  // the last value known to be submitted
		std::uint64_t hostSubmitted = 0;
		std::vector<std::unique_ptr<FrameSlot>> slots;
		std::vector<std::unique_ptr<FrameSlot>> forwardSlots;
		std::uint64_t frames = 0;
		FrameSlot* current = nullptr;
		FrameSlot* signalQueuedFor = nullptr;
		SiteRun* pending = nullptr;  // the site the running epoch body opened
		// A GPU-driven frame's forward run, on its way to its epoch's late pass.
		std::atomic<SiteRun*> handedOff{ nullptr };
		// Native. The late pass may record on the submission thread after the render thread opened the next site, so pending and
		// handedOff may already name another run. Each commit stores its run by lease index for the late pass to take.
		std::mutex siteRunsLock;
		std::vector<std::pair<org::RunRef, SiteRun*>> siteRuns;
		bool wasCpu = false;
		std::shared_ptr<const TargetViews> targets;
		std::atomic<std::uint32_t> nextWorker{ 0 };
		WindowCounters window;
		bool loggedNotAdopted = false, loggedNoLatePass = false, loggedExternal = false;
		rhi::CommandSignaturePtr indirectSignature;  // one indexed draw at IndirectDraw's stride, read past its instance word
		// D3D12 needs one per layout: the instance word, then the indexed draw.
		std::vector<std::pair<rhi::PipelineLayoutHandle, rhi::CommandSignaturePtr>> layoutSignatures;
		std::vector<rhi::TimelinePoint> emitWaits, emitSignals;
	};

	State& S();


	/** @brief A run with only the frame's parts. The site fills in what it binds. */
	SiteRun& NewBareRun(FrameSlot& a_slot, Site a_site);
	/** @brief A main site's run with the commit's shape, resources, main targets and bindings. */
	SiteRun& NewRun(FrameSlot& a_slot, Site a_site, const MainCommitInputs& a_inputs);
	/** @brief Adds a view of a_type's list to a_run. */
	SiteView& NewView(FrameSlot& a_slot, SiteRun& a_run, ViewType a_type, const CullView& a_view, bool a_depthPass);
	/** @brief Render thread. Copies the pool's first indices once a frame, leaving out slots whose geometry is no longer the set's. */
	void TakePoolFirsts(FrameSlot& a_slot, const Draws::IndexPool* a_pool);
	/** @brief Marks the run its epoch body opened. A site outside Direct::ExecuteEpoch calls Emit itself. */
	void SetPending(SiteRun& a_run);
	/** @brief Render thread. Queues a_run's callback. A failure is fatal. */
	void Emit(SiteRun& a_run);
	/**
	 * @brief Render thread. Hands a_run to the late pass of a GPU-driven frame's epoch. Without a_rowsDone its recording would wait for a
	 * later frame while frame start waits on this frame's GPU work, so it returns false and draws nothing.
	 */
	bool HandOffRun(SiteRun& a_run, bool a_rowsDone);


	/** @brief Any thread. Dispatches a_view's culls on DCLF's pool, never on the caller. */
	void DispatchCulls(FrameSlot& a_slot, SiteRun& a_run, SiteView& a_view, const std::shared_ptr<const CullFrame>& a_frame);

	/**
	 * @brief Any thread. Runs a site's dispatches as one frame-critical pool task. Each dispatch wakes a worker,
	 * so this keeps the render thread to one wake instead of one per cull job.
	 */
	void DispatchSiteCulls(FrameSlot& a_slot, std::function<void()> a_dispatch);

	/** @brief Runs a_then when the cull frame is published, unless the slot moved to another frame first. */
	template <class Then>
	void OnCullFrame(FrameSlot& a_slot, Then a_then)
	{
		const std::uint64_t generation = a_slot.generation.load(std::memory_order_acquire);
		AfterCullFrame(a_slot.sceneFrame, [slot = &a_slot, generation, then = std::move(a_then)](const std::shared_ptr<const CullFrame>& a_frame) {
			slot->outstanding.fetch_add(1, std::memory_order_seq_cst);
			if (slot->generation.load(std::memory_order_seq_cst) == generation)
				then(a_frame);
			slot->outstanding.fetch_sub(1, std::memory_order_acq_rel);
		});
	}

	/** @brief Whether a_run draws a_view from indirect runs. */
	inline bool DrawsIndirect(const SiteRun& a_run, const SiteView& a_view)
	{
		if (a_run.record != RecordMode::Indirect || !a_run.slot->indirect.mapped)
			return false;
		switch (a_view.type) {
		case ViewType::Main:
		case ViewType::Sun:
		case ViewType::Spot:
		case ViewType::Point:
		case ViewType::SkyOccluder:
		case ViewType::PrecipOccluder:
		case ViewType::Reflection:
			return true;
		default:
			// Ordered streams and first person, whose colour draws its depth view's batches with other words.
			return false;
		}
	}


	/** @brief Waits for batch a_index. Later batches are kept as they arrive. */
	const CulledBatch& Await(SiteRun& a_run, SiteView& a_view, std::size_t a_index);
	std::vector<const CulledBatch*> AwaitAll(SiteRun& a_run, SiteView& a_view);

	/**
	 * @brief Where a site's callback records on the CS thread. DXVK's buffer gets only barriers and secondary executions.
	 * Everything that binds state goes into secondaries, which start with no state, so nothing leaks between DXVK and the RHI.
	 * The descriptor models still cannot mix. With the RHI on heaps and DXVK on descriptor buffers, NVIDIA fetched garbage descriptors
	 * in these secondaries too, so DXVK and the graph negotiate one model at device creation.
	 */
	struct SiteOut
	{
		rhi::CommandList& primary;
		ListPool& pool;
		rhi::Device& device;
		// Bound in DXVK's buffer before each execution of the site's secondaries.
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		rhi::CommandList* open = nullptr;
		// Native. primary is the epoch's own list with nothing of DXVK's bound, so everything goes straight into it.
		bool direct = false;

		rhi::CommandList& Inline();
		void Flush();
		/**
		 * @brief Binds the run's heaps in DXVK's buffer before each execution. On NVIDIA a secondary's own heap binding did not hold while
		 * DXVK's heap was bound, causing MMU faults. Every execution leaves bindings undefined, so this runs before each one.
		 */
		void BindHeaps();
	};

	/** @brief Records a view's pass, each batch inline as it arrives. Returns the batches in list order. */
	std::vector<const CulledBatch*> RecordViewPass(SiteOut& a_out, SiteRun& a_run, SiteView& a_view, rhi::PassBeginInfo a_begin, RecordBindings const& a_bindings);

	/**
	 * @brief Any thread. Whether the host records late passes inline (native mode). Site epochs are then submitted by the graph and
	 * record into their own primary lists with no secondaries and no external commands.
	 */
	bool SitesInline();

	/**
	 * @brief From a site epoch's late pass, after a full barrier, records the site's draws into a_list. The barrier is needed because
	 * the CPU pipeline passes write memory the graph does not track. False when there is no run or its inputs are incomplete.
	 */
	/**
	 * @brief The images a site pass's recording binds: its declared colours in order, then its depths, each with the view the
	 * recording resolves. A frame still running older graphs binds older images than the resources' current ones (frame placement
	 * places them again, a defrag moves them), so the site draws into these rather than into the views made at commit.
	 */
	struct SiteImages
	{
		struct Image
		{
			rhi::DescriptorSlot view{};
			rhi::ResourceHandle resource{};
		};
		std::vector<Image> colours, depths;
	};

	bool RecordExecutingSite(Site a_site, rhi::CommandList& a_list, const org::RunRef* a_run = nullptr, const SiteImages* a_images = nullptr);

	/** @brief Points a first-person run's views at a_images (SiteImages): the main colours, then its depth target. */
	void UseFirstPersonImages(SiteRun& a_run, const SiteImages& a_images);


	void RecordShadowSite(SiteOut& a_out, SiteRun& a_run);
	void RecordHostedDepthViews(SiteOut& a_out, SiteRun& a_run);
	void RecordFirstPersonSite(SiteOut& a_out, SiteRun& a_run);
	void PrepareFamilies(FrameSlot& a_slot, SiteRun& a_run, const MainCommitInputs& a_inputs);
	void RecordFamilies(SiteOut& a_out, SiteRun& a_run, bool a_depthPass);
}

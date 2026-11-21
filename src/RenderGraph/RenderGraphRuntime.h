#pragma once

#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Frame/SegmentId.h"
#include "RenderGraph/FrameClock.h"

#include <d3d11.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <winrt/base.h>

struct DxvkOrgInteropResourceInfo;
struct DxvkOrgInteropTrackedResource;
struct DxvkOrgInteropRelocation;
struct DxvkOrgInteropTextureWrap;

// The Vulkan handles EmitExternalCommands takes, declared as vulkan_core.h does on 64-bit targets (identical redeclarations),
// so this header stays free of Vulkan includes.
typedef struct VkSemaphore_T* VkSemaphore;
typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef std::uint64_t VkPipelineStageFlags2;

namespace org::services
{
	class ShaderCompiler;
	class PipelineRegistry;
}

namespace rhi
{
	class CommandList;
	class Resource;
	class Timeline;
	struct TimelinePoint;
	template <class TObject>
	class ObjectPtr;
	using TimelinePtr = ObjectPtr<Timeline>;
}

namespace RenderGraph
{
	class BuildManager;
}

namespace org
{
	class ExternalEpochExecution;
	class GraphHost;
	class RenderGraph;
	class Resource;
	class Runtime;
}

/**
* @brief OpenRenderGraph on DXVK's own Vulkan device, or on BasicRHI's own device in native mode.
*
* BasicRHI adopts the VkDevice and queues DXVK created for the game's D3D11 device, so graph passes share memory and the
* graphics queue with the translated D3D11 stream. Epochs are ordered against D3D11 work by their place in DXVK's stream.
* Graph outputs reach D3D11 as wrappers over graph objects (WrapBuffer, WrapTexture). Game resources the graph reads are
* described through DescribeResource.
* The header stays free of ORG, BasicRHI and Vulkan includes, since those need volk ahead of any Vulkan header.
*/
class RenderGraphRuntime
{
public:
	/** @brief Where in the frame an epoch runs. Each epoch runs every pass, so a pass records work only in its own
	*  segment (CurrentSegment) and includes it in its revision. */
	enum class Segment : std::uint32_t
	{
		ZPrepass,      // Drawcall Limit Fix's depth, inside Main::RenderDepth after the world's depth draws, before kPOST_ZPREPASS_COPY
		MainOpaque,    // Drawcall Limit Fix's colour, where the main pass's opaque batches end
		ShadowView,    // Drawcall Limit Fix's shadow casters, inside one shadow view's draw
		SkyOcclusion,  // Drawcall Limit Fix's variant of Skylighting's occlusion map, after the engine's RenderMask
		Reflection,    // Drawcall Limit Fix's water reflection faces, after the engine's face render (TESWaterReflections::Update)
		Forward, // Drawcall Limit Fix's forward epoch: blended Lighting, at the engine's blended draw after the composite
		FirstPersonDepth, // Drawcall Limit Fix's first-person depth, at RenderDepth's first-person draw, before kPOST_ZPREPASS_COPY
		FirstPerson, // Drawcall Limit Fix's first-person colour, at Main::RenderFirstPersonView's accumulator render
		WaterMask,     // Drawcall Limit Fix's water mask, at the engine's water mask batch in FinishAccumulating
		WaterSurface,  // Drawcall Limit Fix's water surface, at the engine's water batch in FinishAccumulatingPostResolveDepth
		// The CS stages' epochs, one per RenderGraph::Frame::SegmentId in that order.
		Declared,
		Count = Declared + RenderGraph::Frame::kSegments
	};

	/** @brief The epoch's segment of CS stage a_segment (a RenderGraph::Frame::SegmentId value). */
	static constexpr Segment StageSegment(std::uint32_t a_segment) { return static_cast<Segment>(static_cast<std::uint32_t>(Segment::Declared) + a_segment); }

	static RenderGraphRuntime& Get();

	/** @brief Game frames the graph's work may run behind the render thread. Revision-driven epochs publish this many copies.
	*  Fewer than DXVK's frame latency would stall the render thread every few frames on those copies. */
	static constexpr std::uint32_t kGameFramesInFlight = RenderGraph::FrameClock::kSlots;

	/** @brief The first capacity of run-indexed services, which grow past it when more runs are in flight. */
	static constexpr std::uint32_t kRunCapacity = 256;

	/** @brief The ORG epoch a segment's passes declare (ExternalPassDesc::Epoch). Each segment is an ORG host epoch. */
	static std::uint32_t EpochOf(Segment a_segment);

	/**
	* @brief Hooks into every epoch's submission, for work another module keeps in some epochs' graphs. Set once before the first frame.
	* starting runs before any commit. commit runs first inside the epoch's commit, with CurrentRun as the epoch's run.
	*/
	struct EpochHooks
	{
		void (*starting)(Segment) = nullptr;
		void (*commit)(Segment) = nullptr;
	};
	static void SetEpochHooks(EpochHooks a_hooks);

	/** @brief Asks DXVK to enable the device features BasicRHI needs. Call before the D3D11 device exists. */
	static void RequestDeviceFeatures(HMODULE a_dxvkD3D11);

	/** @brief The frame's epochs as Segment values: the frame order, which is also the async ticket order, and which are revision-driven. */
	struct FrameConfig
	{
		std::vector<std::uint32_t> epochOrder, revisionEpochs;
	};
	/** @brief Sets the frame's epochs from DCLF's schedule, once before Initialize. Without it Initialize fails, which is fatal in graph mode. */
	static void ConfigureFrame(FrameConfig a_config);

	/** @brief Adopts DXVK's Vulkan device. Safe to call repeatedly; returns whether the runtime is active. */
	bool Initialize();

	/**
	* @brief Makes ORG's runtime on a_device with CS's settings (pass statistics, alias logging) applied first, since the runtime
	* reads them when it is made. Native mode makes it with its device (stage 3), guest mode in Initialize. A failure throws.
	*/
	static std::shared_ptr<org::Runtime> CreateOrgRuntime(const rhi::DevicePtr& a_device);

	/** @brief Retires all graph work and releases the adopted device (never destroys it). */
	void Shutdown();

	/**
	* @brief Whether the graph is set up. In graph mode a failed start or a fault is fatal, so this is false only before setup.
	* Outside graph mode (CS_ORG=features) a fault turns it off and callers use D3D11.
	*/
	bool IsActive() const;

	/** @brief The adopted Vulkan device's PCI vendor (0x1002 AMD, 0x10DE NVIDIA, 0x8086 Intel); 0 before a device was adopted. */
	static std::uint32_t VendorId();

	/** @brief Why the runtime is inactive (empty when active). */
	const std::string& GetDisabledReason() const;

	/** @brief The persistent graph host; null when inactive. */
	org::GraphHost* Host();

	/** @brief The host's build manager. Null before the host exists. Request rebuilds through RenderGraph::Builds instead. */
	RenderGraph::BuildManager* Builds();

	/** @brief Render thread, before blocking on GPU work. Flushes the immediate context if graph work in DXVK's stream is unsubmitted,
	*  since otherwise the wait never ends. Returns the stream submissions that were pending. */
	std::uint64_t FlushPendingStreamWork();
	/** @brief The host's frame-wait timeline, signalled from the upload queue after per-frame copies. Null without an upload queue.
	*  Its signal must be submitted before Present enters DXVK, or AMD's windowed present deadlocks on DXVK's submission lock. */
	std::shared_ptr<rhi::TimelinePtr> FrameWaitTimeline() const;
	/** @brief Retains the adopted BasicRHI wrapper for owned external bindings. */
	std::shared_ptr<const void> DeviceOwner() const;

	/** @brief Runs one graph epoch at the current point of the D3D11 stream. Queue the frame's uploads in a_beforePrepare.
	*  Returns false when the graph or this epoch failed, and callers fall back to D3D11. */
	bool ExecuteEpoch(Segment a_segment, const std::function<void(org::RenderGraph&)>& a_beforePrepare = {},
		std::shared_ptr<const void> a_resourceOwner = {});

	/** @brief What TryExecuteEpoch did. */
	enum class EpochResult : std::uint8_t
	{
		Submitted,    // the epoch was submitted
		Skipped, // a revision-driven epoch ran more often since Present than it has published copies. Nothing failed
		EpochFailed, // only this epoch's graph failed. The graph runs on for the others
		Stopped,      // inactive, or the graph failed and stopped
	};

	/** @brief ExecuteEpoch, telling a skipped run from a failure. */
	EpochResult TryExecuteEpoch(Segment a_segment, const std::function<void(org::RenderGraph&)>& a_beforePrepare = {},
		std::shared_ptr<const void> a_resourceOwner = {});

	/** @brief The render owner, inside the frame's adoption point (Frame::Driver::OnBegin) only. Hands each epoch whose graph failed
	*  since the last call to its owner. */
	void PollEpochFailures();

	/** @brief Owner thread, at a frame boundary. Attaches finished builds and hands requested ones to the build thread.
	*  Use this, never the host's BuildIfRequested, so a failed host thread stops the graph instead of throwing into the frame. */
	bool BuildIfRequested(std::string_view a_caller = "build point");

	/**
	* @brief Owner thread. Whether no lane-only epoch (DCLF's) is requested or building, and each has its first graph once builds have
	* an input. A staged build is settled. True while inactive.
	*/
	bool BuildsSettled() const;

	/** @brief Owner thread. Whether a requested rebuild leaves frames without the graph until built. Staged CS stages never do. */
	bool RebuildRequestedUnstaged() const;

	/** @brief Owner thread, at setup. Builds every requested graph now before the first world frame, else acts as a build point. */
	bool BuildSetupGraphs(std::string_view a_caller);

	/** @brief Render thread. Submits an external epoch whose recording the caller executes in DXVK's command buffer.
	*  Null when inactive. Throws as the host's SubmitEpoch does. */
	std::shared_ptr<org::ExternalEpochExecution> ExecuteExternalEpoch(Segment a_segment, const std::function<void(org::RenderGraph&)>& a_beforePrepare,
		std::shared_ptr<const void> a_resourceOwner = {});
	/** @brief Render thread. Ends an external epoch after the caller emitted it, adding the stream's wait for its compute and copy work.
	*  a_emitted means every segment went into DXVK's stream. The wait must not share a list with the segments it waits for. */
	void CloseExternalEpoch(bool a_emitted);
	/** @brief Render thread. Signals a_execution's segments from a_first on without running them, and notes them in the telemetry. */
	void SignalExecutionUnexecuted(const org::ExternalEpochExecution& a_execution, std::size_t a_first, const char* a_label);
	/** @brief Render thread: ends DXVK's command list (a D3D11 Flush), for a segment that must start a submission of its own. */
	void SplitStream();
	/** @brief Any thread, for a stall report. Recent off-stream submissions, stream points and flushes with semaphore values. Read racily. */
	std::string DescribeStreamOrder(std::size_t a_events = 96) const;

	/** @brief The render thread's ExecuteEpoch time since startup, split three ways. A feature diffs it across its own call. */
	struct EpochCpuTotals
	{
		double inputsUs = 0.0;    // the feature's a_beforePrepare
		double submitUs = 0.0;    // the submission proper: the ticket (and any wait for it), the queue submission, the stream's close
		double overheadUs = 0.0;  // the rest: releases, the ticket's check, upload recording, the native flush, completions, posts
		std::uint64_t epochs = 0;
	};
	const EpochCpuTotals& RenderThreadEpochTotals() const { return epochTotals; }

	/** @brief The segment of the epoch being executed (valid while passes prepare and record). */
	Segment CurrentSegment() const { return segment; }


	/**
	* @brief The render thread's pacing point. DXVK's frame-latency wait is off, so without this the slot rings stall mid-frame in bursts.
	* The GPU may trail by CS_FRAME_LATENCY - 1 frames, default 2. CS_FRAME_LATENCY=0 turns it off. No-op while frame generation paces.
	*/
	void SignalFramePace(bool a_active);
	void WaitFramePace();
	/**
	* @brief Renderer::Begin's detour (Frame/FrameDriver.cpp), after the render owner's claim and before Frame::Driver::OnBegin.
	* Moves the frame clock's beginFrame (RenderGraph::FrameClock), so Begin reads the frame it starts. On the render owner it also
	* measures how far the frame runs ahead of the GPU, logged every 600 frames. Any thread that begins a frame.
	*/
	static void OnRendererBegin();
	/** @brief Render thread, after a successful present. Seals everything submitted so far, the points a mid-frame producer may wait for.
	*  Moves the frame clock's presentSerial. */
	void SealPresented();
	/** @brief Makes the calling thread the owner of the graph's frame upload instance, when rendering changes threads. */
	void MoveOwnerThread();

	/** @brief Native submission thread. Blocks until every epoch handed to it is on its queue. Free when nothing is queued.
	*  Call before the render owner's own queue work that must follow them, such as a present or a GPU wait. */
	void DrainSubmissions(const char* a_why, bool a_anyThread = false);

	/** @brief Whether the present goes onto the submission thread's queue. Render owner only. */
	bool QueuesPresent() const;

	/** @brief Render owner. Runs a_closure on the submission thread after the epochs queued so far, or at once without one. */
	void QueueOnSubmitThread(std::function<void()> a_closure);

	/** @brief Render owner. A wait the next present runs just before the swap chain present.
	*  A present can block on the frame's GPU work while holding the submission lock, so what that work waits for must go first. */
	void WaitBeforePresent(std::function<void()> a_wait);
	/** @brief The waits the next present runs (WaitBeforePresent), taken by the thread that queues or makes the present. */
	std::vector<std::function<void()>> TakePresentWaits();

	/** @brief Render thread: the segments submitted since the last call, one bit per Segment. */
	std::uint64_t TakeSubmittedSegments();

	/** @brief The Vulkan resource behind a D3D11 buffer, texture or SRV, marked stable in DXVK. Fails for mappable buffers.
	*  Keep the D3D11 object referenced while using it. Synchronizes with DXVK's worker, so resolve once and cache. */
	bool DescribeResource(IUnknown* a_object, DxvkOrgInteropResourceInfo& a_info);
	/** @brief DescribeResource for many objects with one sync. False when DXVK lacks the batched export. */
	bool DescribeResources(std::span<IUnknown* const> a_objects, std::span<DxvkOrgInteropResourceInfo> a_infos, std::span<HRESULT> a_results);

	/** @brief Whether imports can be tracked instead of pinned. Fixed once the runtime is up. False when DXVK lacks the exports. */
	bool PinlessImports() const;
	/** @brief DescribeResources without pinning. DXVK may move the resource, reported by PollRelocations.
	*  End each success with UntrackResources while the D3D11 object is still referenced. */
	bool TrackResources(std::span<IUnknown* const> a_objects, std::span<DxvkOrgInteropTrackedResource> a_resources, std::span<HRESULT> a_results);
	/** @brief Ends one import of each object's resource (any thread). */
	void UntrackResources(std::span<IUnknown* const> a_objects);
	/** @brief Takes reported relocations, oldest first (any thread); each one's storage must be released. */
	std::uint32_t PollRelocations(std::span<DxvkOrgInteropRelocation> a_relocations);
	/** @brief Releases relocations' old storage once the GPU no longer uses it (any thread). */
	void ReleaseStorage(std::span<void* const> a_storages);

	/** @brief A timeline semaphore point an external recording waits for or signals (stages 0: all commands). */
	struct ExternalCommandsWait
	{
		VkSemaphore semaphore;
		std::uint64_t value;
		VkPipelineStageFlags2 stages;
	};
	/**
	* @brief Render thread. Queues a_record into DXVK's own command buffer in D3D11 stream order, outside render passes. False when unsupported.
	* The recording finds every image in its DXVK layout and must leave it there. a_bindsState makes DXVK forget all bindings after.
	*/
	bool EmitExternalCommands(void (*a_record)(void* a_user, VkCommandBuffer a_commandBuffer), void* a_user,
		std::span<const ExternalCommandsWait> a_waits, std::span<const ExternalCommandsWait> a_signals, bool a_bindsState,
		const char* a_label, bool a_fullBarrier = false);
	/** @brief An ORG timeline point as the semaphore EmitExternalCommands takes (the frame-wait timeline's too); none when it has none. */
	static ExternalCommandsWait NativeTimelinePoint(const rhi::TimelinePoint& a_point);
	/**
	* @brief Render thread. EmitExternalCommands with BasicRHI types, in every mode. a_record starts with nothing bound and runs once on success.
	* Guest mode adopts DXVK's command buffer. Native mode submits its own list between full barriers. Epoch work belongs in a pass instead.
	*/
	bool EmitCommands(void (*a_record)(void* a_user, rhi::CommandList& a_commands), void* a_user, const rhi::TimelinePoint* a_waits,
		std::uint32_t a_waitCount, const rhi::TimelinePoint* a_signals, std::uint32_t a_signalCount, bool a_bindsState, const char* a_label,
		bool a_fullBarrier = false);
	/** @brief Whether a RecordAtExecution pass of the epoch records at execution, in DXVK's stream or inline in native mode. */
	bool RecordsAtExecution(std::uint32_t a_epoch) const;
	/** @brief The async epochs whose graphics work goes into DXVK's own command stream. Empty in native mode. */
	std::vector<std::uint32_t> InStreamEpochs() const;

	/** @brief Runtime DXC compilation to SPIR-V, saved in the shared shader store. Null without DXC beside the DXVK DLLs. */
	org::services::ShaderCompiler* ShaderCompiler();

	/** @brief Data/Shaders and every .hlsl/.hlsli in it, listed again after each reload that added or removed files. */
	static constexpr const char* kShaderDirectory = "Data/Shaders";
	static const std::vector<std::filesystem::path>& ShaderSourceFiles();

	/** @brief Shader hot reload. Every live pipeline built from Data/Shaders registers here and binds through its slot.
	*  Lives for the whole process, since a rebuild may complete at teardown. */
	static org::services::PipelineRegistry* PipelineRegistry();
	/**
	* @brief Any thread. Rebuilds the live pipelines built from a_changedFile, or all, off the render thread. Their new versions
	* are published at a new pipeline generation, every graph is built again with them, and the frame revision that carries the
	* new ShaderGeneration adopts the rebuilt graphs (RenderGraph/PipelineVersions.h).
	*/
	static void RequestShaderReload(std::string_view a_changedFile = {}, bool a_force = false);
	/** @brief Any thread: the next a_epochs completed epochs log each graphics timestamp range in GPU order ("[TSDUMP]"). */
	static void RequestTimestampDump(std::uint32_t a_epochs);

	/** @brief Wraps a materialized graph-owned buffer as a D3D11 buffer (DEFAULT usage, no CPU access). */
	winrt::com_ptr<ID3D11Buffer> WrapBuffer(org::Resource& a_buffer, const D3D11_BUFFER_DESC& a_desc);

	/**
	* @brief Wraps a BasicRHI image the caller owns as a D3D11 texture. a_info.image.layout is its resting layout between DXVK's commands.
	* DXVK zeroes it, or uploads a_initialData. Any thread. Null when inactive or rejected.
	*/
	winrt::com_ptr<ID3D11Resource> WrapTexture(const rhi::Resource& a_image, const DxvkOrgInteropTextureWrap& a_wrap, DxvkOrgInteropResourceInfo& a_info,
		const char* a_name, const D3D11_SUBRESOURCE_DATA* a_initialData = nullptr,
		RenderGraph::Engine::Native::WrapFlags a_flags = RenderGraph::Engine::Native::WrapFlags::None);

	/** @brief Render thread. Holds a_owner until the GPU finishes every D3D11 command issued so far, so exports outlive D3D11's last use. */
	void ReleaseAfterD3D11(std::shared_ptr<const void> a_owner);

	/** @brief Totals, and with a_log logs, each segment's GPU time from ORG's pass timestamps over a_frames frames. Render thread.
	*  Exclusive time runs from the previous pass's end, so overlapping passes are not counted twice. */
	void ReportGpuTimings(std::uint32_t a_frames, bool a_log);

	/** @brief One line per segment from the last ReportGpuTimings, for the menu; empty before the first. */
	const std::string& GpuTimingSummary() const { return gpuTimingSummary; }

	~RenderGraphRuntime();

private:
	RenderGraphRuntime() = default;
	/** @brief Every frame, serves the registry's reload, raises ShaderGeneration for new versions and releases unpinned ones. Render thread. */
	void ServicePipelineReload();
	/** @brief Render owner, at Renderer::Begin. Measures how far frame a_frame runs ahead of the GPU, logged every 600 frames. */
	void NoteFrameBegin(std::uint64_t a_frame);
	/** @brief The host's thread, when an epoch completes: moves the frame clock's gpuCompleted forward to a_frame. */
	static void NoteGpuCompleted(std::uint64_t a_frame);
	/** @brief Every frame, drops ReleaseAfterD3D11 owners whose query completed. Render thread. */
	void ServiceD3D11Releases();
	/** @brief Routes an epoch failure to the epoch's owner. */
	void OnEpochFailed(std::uint32_t a_epoch, bool a_keptGraph, const std::string& a_error);
	/** @brief Runs a host build call. A throw stops the graph and returns false. */
	bool GuardedBuild(const std::function<bool()>& a_build);

	struct Impl;
	std::unique_ptr<Impl> impl;
	std::string disabledReason = "not initialized";
	std::string gpuTimingSummary;
	EpochCpuTotals epochTotals;
	std::uint64_t buildPoints = 0;  // BuildIfRequested's calls
	std::uint64_t buildPointsAtSeal = 0;  // buildPoints at the last SealPresented
	std::uint64_t pipelineGenerationPublished = 0;  // the registry's newest generation its Service published. Render thread
	std::uint64_t pipelineGenerationRaised = 0;     // the last ShaderGeneration that ServicePipelineReload wrote. Render thread
	Segment segment = Segment::Count;
	bool attempted = false;
	FrameConfig frameConfig;  // ConfigureFrame's (frameConfigured)
	bool frameConfigured = false;
};

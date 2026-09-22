#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraphRuntime.h"

#include "DxvkLoader.h"
#include "Features/Upscaling/DXVKInteropInterfaces.h"
#include "Globals.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "State.h"

#include <OpenRenderGraph/PersistentGraphHost.h>
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#	include <ORGModuleServices/ShaderCompiler.h>
#endif
#include <Resources/Resource.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
	// Everything BasicRHI's Vulkan backend needs beyond what DXVK enables for itself.
	// DXVK already requires the rest (timeline semaphores, buffer device address,
	// descriptor indexing, scalar block layout, dynamic rendering, maintenance5).
	// DXVK enables vulkanMemoryModel, which makes the Device-scope atomics DXC emits
	// invalid unless vulkanMemoryModelDeviceScope is enabled as well.
	// Drawcall Limit Fix draws through device-generated commands (optional: without it the graph
	// runs, and DCLF stays off).
	constexpr DxvkOrgInteropFeature kRequestedFeatures[] = {
		{ VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, "descriptorHeap" },
		{ nullptr, "vulkanMemoryModelDeviceScope" },
		{ VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME, "deviceGeneratedCommands" },
	};

	bool EnvDisabled()
	{
		char buf[8] = {};
		return GetEnvironmentVariableA("CS_ORG", buf, sizeof(buf)) && buf[0] == '0';
	}

	// Diagnostic: CS_ORG=features requests the interop device features but never creates the
	// graph, separating side effects of the device configuration from those of the graph's work.
	bool EnvEquals(const char* a_name, const char* a_value)
	{
		char buf[32] = {};
		return GetEnvironmentVariableA(a_name, buf, sizeof(buf)) && std::strcmp(buf, a_value) == 0;
	}

	bool EnvFeaturesOnly()
	{
		return EnvEquals("CS_ORG", "features");
	}

	template <class T>
	T ResolveExport(HMODULE a_module, const char* a_name)
	{
		return a_module ? reinterpret_cast<T>(reinterpret_cast<void*>(::GetProcAddress(a_module, a_name))) : nullptr;
	}

	bool HasExtension(const DxvkOrgInteropDeviceInfo& a_info, const char* a_name)
	{
		for (uint32_t i = 0; i < a_info.enabledExtensionCount; ++i)
			if (a_info.enabledExtensions[i] && std::strcmp(a_info.enabledExtensions[i], a_name) == 0)
				return true;
		return false;
	}
}

struct RenderGraphRuntime::Impl
{
	winrt::com_ptr<IDXGIVkInteropDevice> interop;
	PFN_dxvkCreateBufferFromVkBuffer createBufferFromVkBuffer = nullptr;
	PFN_dxvkSetDeviceTeardownCallback setTeardownCallback = nullptr;
	PFN_dxvkEnqueueInteropSubmission enqueueSubmission = nullptr;
	PFN_dxvkGetInteropResourceInfo getResourceInfo = nullptr;
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	std::unique_ptr<org::services::ShaderCompiler> shaderCompiler;
#endif
	PFN_vkQueueSubmit2 queueSubmit2 = nullptr;
	rhi::DevicePtr device;
	std::unique_ptr<org::PersistentGraphHost> host;
	std::atomic<bool> faulted = false;

	// The thread that drives the D3D11 immediate context (the one running epochs).
	// Its submissions go into DXVK's command stream; other threads submit directly.
	std::atomic<DWORD> streamThread = 0;
	// Submissions handed to DXVK's stream, and how many of those reached the queue.
	std::atomic<uint64_t> streamEnqueued = 0;
	std::atomic<uint64_t> streamSubmitted = 0;

	// CS_ORG_EPOCH_STATS=1: render-thread CPU time per epoch, logged every kEpochStatsWindow epochs.
	static constexpr uint32_t kEpochStatsWindow = 600;
	bool epochStats = false;
	uint32_t epochCount = 0;
	double epochTotalUs = 0.0;
	double epochMaxUs = 0.0;

	// ORG's pass timestamps, per segment. An epoch is one host frame; the segment it ran is remembered by
	// frame number until its timestamps come back, framesInFlight frames later.
	static constexpr std::size_t kSegments = 5;
	static constexpr std::size_t kSegmentRing = 16;
	struct PassTime
	{
		double inclusiveMs = 0.0;
		double exclusiveMs = 0.0;
	};
	struct SegmentTime
	{
		std::map<std::string, PassTime> passes;
		double spanMs = 0.0;
		std::uint32_t epochs = 0;
	};
	std::array<Segment, kSegmentRing> frameSegments{};
	std::array<SegmentTime, kSegments> segmentTimes{};
	std::uint32_t timedEpochs = 0;
	std::uint32_t completedFrames = 0;  // callbacks, timed or not: tells "no readback" from "no timestamps"

	void OnCompletedFrame(std::uint64_t a_frameNumber, const org::runtime::IStatisticsService& a_stats)
	{
		const auto segmentIndex = static_cast<std::size_t>(frameSegments[a_frameNumber % kSegmentRing]);
		if (segmentIndex >= kSegments)
			return;
		const auto& stats = a_stats.GetPassStats();
		const auto& names = a_stats.GetPassNames();
		const double toMs = a_stats.GetGpuTicksToMilliseconds();
		const std::uint64_t serial = a_stats.GetFrameSerial();
		++completedFrames;
		static std::uint32_t described = 0;
		if (described < 3) {
			++described;
			std::size_t matching = 0, zeroTicks = 0;
			std::uint64_t newest = 0;
			for (const auto& pass : stats) {
				newest = (std::max)(newest, pass.gpuSampleSerial);
				matching += pass.gpuSampleSerial == serial;
				zeroTicks += pass.gpuSampleSerial == serial && !pass.gpuBeginTick;
			}
			logger::info("[ORG] completed frame {}: {} passes registered, statistics serial {}, {} read back for it ({} without ticks), newest sample serial {}, {:.6f} ms per tick",
				a_frameNumber, stats.size(), serial, matching, zeroTicks, newest, toMs);
		}
		struct Sample
		{
			std::size_t pass;
			std::uint64_t begin;
			std::uint64_t end;
		};
		std::vector<Sample> samples;
		for (std::size_t i = 0; i < stats.size() && i < names.size(); ++i) {
			if (stats[i].gpuSampleSerial == serial && stats[i].gpuEndTick >= stats[i].gpuBeginTick && stats[i].gpuBeginTick)
				samples.push_back({ i, stats[i].gpuBeginTick, stats[i].gpuEndTick });
		}
		if (samples.empty() || toMs <= 0.0)
			return;
		std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) { return a.begin < b.begin; });
		auto& segmentTime = segmentTimes[segmentIndex];
		std::uint64_t previousEnd = samples.front().begin;
		for (const auto& sample : samples) {
			auto& pass = segmentTime.passes[names[sample.pass]];
			pass.inclusiveMs += double(sample.end - sample.begin) * toMs;
			const std::uint64_t from = (std::max)(previousEnd, sample.begin);
			pass.exclusiveMs += sample.end > from ? double(sample.end - from) * toMs : 0.0;
			previousEnd = (std::max)(previousEnd, sample.end);
		}
		segmentTime.spanMs += double(previousEnd - samples.front().begin) * toMs;
		++segmentTime.epochs;
		++timedEpochs;
	}

	void RecordEpoch(std::chrono::steady_clock::duration a_elapsed)
	{
		const double us = std::chrono::duration<double, std::micro>(a_elapsed).count();
		epochTotalUs += us;
		epochMaxUs = (std::max)(epochMaxUs, us);
		if (++epochCount < kEpochStatsWindow)
			return;
		logger::info("[ORG] Epoch CPU ({}): avg {:.1f} us, max {:.1f} us over {} epochs",
			enqueueSubmission ? "stream" : "flush", epochTotalUs / epochCount, epochMaxUs, epochCount);
		epochCount = 0;
		epochTotalUs = 0.0;
		epochMaxUs = 0.0;
	}

	static void LockQueue(void* a_user, VkQueue)
	{
		static_cast<Impl*>(a_user)->interop->LockSubmissionQueue();
	}

	static void UnlockQueue(void* a_user, VkQueue)
	{
		static_cast<Impl*>(a_user)->interop->ReleaseSubmissionQueue();
	}

	static void OnStreamSubmitted(void* a_user, VkResult a_result)
	{
		auto* self = static_cast<Impl*>(a_user);
		if (a_result != VK_SUCCESS && !self->faulted.exchange(true))
			logger::error("[ORG] DXVK failed to submit render graph work ({}); disabling the graph", static_cast<int>(a_result));
		self->streamSubmitted.fetch_add(1, std::memory_order_release);
	}

	// Blocks until everything handed to DXVK's stream has reached the queue, or the timeout expires.
	bool WaitForStreamSubmissions(std::chrono::milliseconds a_timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + a_timeout;
		const uint64_t target = streamEnqueued.load(std::memory_order_acquire);
		for (uint64_t done = streamSubmitted.load(std::memory_order_acquire); done < target; done = streamSubmitted.load(std::memory_order_acquire)) {
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			// Only reached by submissions from threads other than the D3D11 one, which the
			// graph does not issue per frame, so a short poll costs nothing in steady state.
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
		return true;
	}

	// BasicRHI hands every queue submission here instead of calling vkQueueSubmit.
	static const char* SegmentLabel(Segment a_segment)
	{
		switch (a_segment) {
		case Segment::LightCulling:
			return "CS LLF: light culling";
		case Segment::ZPrepass:
			return "CS DCLF: Z-prepass";
		case Segment::MainOpaque:
			return "CS DCLF: main opaque";
		case Segment::DebugView:
			return "CS DCLF: debug view";
		case Segment::ShadowView:
			return "CS DCLF: shadow view";
		}
		return "CS render graph";
	}

	static VkResult Submit(void* a_user, VkQueue a_queue, const VkSubmitInfo2& a_submit)
	{
		auto* self = static_cast<Impl*>(a_user);
		if (::GetCurrentThreadId() == self->streamThread.load(std::memory_order_relaxed)) {
			// In D3D11 stream order: after every D3D11 command issued so far, before every later
			// one. Nothing is waited for here.
			DxvkOrgInteropSubmission submission{};
			submission.version = DXVK_ORG_INTEROP_VERSION;
			submission.waitCount = a_submit.waitSemaphoreInfoCount;
			submission.waits = a_submit.pWaitSemaphoreInfos;
			submission.commandBufferCount = a_submit.commandBufferInfoCount;
			submission.commandBuffers = a_submit.pCommandBufferInfos;
			submission.signalCount = a_submit.signalSemaphoreInfoCount;
			submission.signals = a_submit.pSignalSemaphoreInfos;
			submission.onSubmitted = &Impl::OnStreamSubmitted;
			submission.user = self;
			// Nsight and RenderDoc group the epoch's command buffers under this (DXVK adds it only
			// while a capture tool is attached).
			submission.label = SegmentLabel(RenderGraphRuntime::Get().CurrentSegment());
			self->streamEnqueued.fetch_add(1, std::memory_order_acq_rel);
			if (FAILED(self->enqueueSubmission(globals::d3d::device, &submission))) {
				self->streamEnqueued.fetch_sub(1, std::memory_order_acq_rel);
				return VK_ERROR_UNKNOWN;
			}
			return VK_SUCCESS;
		}

		// Another thread: keep the graph's submissions in order by letting everything already in
		// DXVK's stream reach the queue first, then submit directly under DXVK's queue lock.
		if (!self->WaitForStreamSubmissions(std::chrono::seconds(10))) {
			logger::error("[ORG] Timed out waiting for queued render graph submissions");
			return VK_TIMEOUT;
		}
		self->interop->LockSubmissionQueue();
		const VkResult result = self->queueSubmit2(a_queue, 1, &a_submit, VK_NULL_HANDLE);
		self->interop->ReleaseSubmissionQueue();
		return result;
	}

	static void OnDxvkTeardown(void*, VkDevice)
	{
		logger::info("[ORG] DXVK is destroying its device; retiring the render graph first");
		RenderGraphRuntime::Get().Shutdown();
	}
};

RenderGraphRuntime& RenderGraphRuntime::Get()
{
	static RenderGraphRuntime s_runtime;
	return s_runtime;
}

void RenderGraphRuntime::RequestDeviceFeatures(HMODULE a_dxvkD3D11)
{
	if (EnvDisabled())
		return;
	auto request = ResolveExport<PFN_dxvkRequestDeviceFeatures>(a_dxvkD3D11, "dxvkRequestDeviceFeatures");
	if (!request) {
		logger::warn("[ORG] dxvkRequestDeviceFeatures is unavailable - this DXVK build cannot host the render graph");
		return;
	}
	const DxvkOrgInteropFeatureRequest desc{ DXVK_ORG_INTEROP_VERSION, static_cast<uint32_t>(std::size(kRequestedFeatures)), kRequestedFeatures };
	if (FAILED(request(&desc)))
		logger::warn("[ORG] DXVK rejected the render graph device feature request");
}

bool RenderGraphRuntime::Initialize()
{
	if (impl)
		return true;
	if (attempted)
		return false;
	attempted = true;

	auto disable = [&](std::string a_reason) {
		disabledReason = std::move(a_reason);
		logger::info("[ORG] Render graph disabled: {}", disabledReason);
		impl.reset();
		return false;
	};

	if (EnvDisabled())
		return disable("CS_ORG=0");
	if (EnvFeaturesOnly())
		return disable("CS_ORG=features (device features requested, graph not created)");
	if (!DxvkLoader::IsLoaded())
		return disable("DXVK is not loaded (native D3D11)");

	HMODULE d3d11 = ::GetModuleHandleW(L"dxvk_d3d11.dll");
	auto getInfo = ResolveExport<PFN_dxvkGetInteropDeviceInfo>(d3d11, "dxvkGetInteropDeviceInfo");
	auto state = std::make_unique<Impl>();
	state->createBufferFromVkBuffer = ResolveExport<PFN_dxvkCreateBufferFromVkBuffer>(d3d11, "dxvkCreateBufferFromVkBuffer");
	state->setTeardownCallback = ResolveExport<PFN_dxvkSetDeviceTeardownCallback>(d3d11, "dxvkSetDeviceTeardownCallback");
	if (!getInfo || !state->createBufferFromVkBuffer || !state->setTeardownCallback)
		return disable("the DXVK build lacks the render graph interop exports");
	// Optional: game resources for the graph (Drawcall Limit Fix).
	state->getResourceInfo = ResolveExport<PFN_dxvkGetInteropResourceInfo>(d3d11, "dxvkGetInteropResourceInfo");
	// Optional: without it, each epoch flushes and waits for DXVK's command stream instead.
	state->enqueueSubmission = ResolveExport<PFN_dxvkEnqueueInteropSubmission>(d3d11, "dxvkEnqueueInteropSubmission");
	if (EnvEquals("CS_ORG_SUBMIT", "flush"))
		state->enqueueSubmission = nullptr;
	state->epochStats = EnvEquals("CS_ORG_EPOCH_STATS", "1");

	auto d3dDevice = globals::d3d::device;
	if (!d3dDevice || FAILED(d3dDevice->QueryInterface(__uuidof(IDXGIVkInteropDevice), state->interop.put_void())))
		return disable("IDXGIVkInteropDevice is unavailable");

	DxvkOrgInteropDeviceInfo info{};
	info.version = DXVK_ORG_INTEROP_VERSION;
	if (FAILED(getInfo(d3dDevice, &info)))
		return disable("DXVK could not describe its Vulkan device");
	if (!HasExtension(info, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME)) {
		// The one cause seen in practice, named so the menu does not leave it to be guessed.
		if (::GetModuleHandleW(L"renderdoc.dll"))
			return disable("the device was created without VK_EXT_descriptor_heap: RenderDoc is loaded, and RenderDoc does not support "
						   "descriptor heaps yet (its capture layer hides the extension). Turn off RenderDoc capture to use the render graph.");
		return disable("the device was created without VK_EXT_descriptor_heap (the driver or a Vulkan layer does not expose it)");
	}

	rhi::vulkan::AdoptedVulkanDeviceInfo adopt{};
	adopt.getInstanceProcAddr = info.getInstanceProcAddr;
	adopt.instance = info.instance;
	adopt.instanceApiVersion = info.instanceApiVersion;
	adopt.physicalDevice = info.physicalDevice;
	adopt.device = info.device;
	adopt.enabledDeviceExtensions = info.enabledExtensions;
	adopt.enabledDeviceExtensionCount = info.enabledExtensionCount;
	// Whether VK_EXT_debug_utils is on, so that BasicRHI names objects and labels passes only then.
	adopt.enabledInstanceExtensions = info.enabledInstanceExtensions;
	adopt.enabledInstanceExtensionCount = info.enabledInstanceExtensionCount;
	adopt.enabledFeatureChain = info.enabledFeatures;
	adopt.queues[0] = { info.graphicsQueue, info.graphicsQueueFamily, info.graphicsQueueIndex };
	adopt.submissionHooks = { state.get(), &Impl::LockQueue, &Impl::UnlockQueue, nullptr };
	if (state->enqueueSubmission) {
		auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(info.getInstanceProcAddr(info.instance, "vkGetDeviceProcAddr"));
		state->queueSubmit2 = getDeviceProcAddr ? reinterpret_cast<PFN_vkQueueSubmit2>(getDeviceProcAddr(info.device, "vkQueueSubmit2")) : nullptr;
		if (state->queueSubmit2) {
			adopt.submissionHooks.submit = &Impl::Submit;
			state->streamThread = ::GetCurrentThreadId();
		} else {
			state->enqueueSubmission = nullptr;
		}
	}
	if (rhi::vulkan::AdoptVulkanDevice(adopt, state->device) != rhi::Result::Ok || !state->device)
		return disable("BasicRHI could not adopt DXVK's Vulkan device");

	try {
		org::PersistentGraphHost::Desc desc{};
		desc.device = state->device.Get();
		desc.backend = rhi::Backend::Vulkan;
		// Everything DXVK submitted before an epoch and everything it submits after
		// is ordered against the graph by these barriers (same queue, submission order).
		desc.queueBoundary = { .entry = true, .exit = true };
		desc.framesInFlight = 3;
		state->host = std::make_unique<org::PersistentGraphHost>(std::move(desc));
	} catch (const std::exception& e) {
		return disable(std::string("graph host creation failed: ") + e.what());
	}

	state->setTeardownCallback(&Impl::OnDxvkTeardown, nullptr);
	state->host->SetCompletedFrameCallback([self = state.get()](std::uint64_t a_frameNumber, const org::runtime::IStatisticsService& a_stats) {
		self->OnCompletedFrame(a_frameNumber, a_stats);
	});
	impl = std::move(state);
	disabledReason.clear();
	logger::info("[ORG] Render graph adopted DXVK's Vulkan device (queue family {}, index {}); submissions {}",
		info.graphicsQueueFamily, info.graphicsQueueIndex,
		impl->enqueueSubmission ? "go through DXVK's command stream" : "flush DXVK each epoch");
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	{
		// DXC ships beside the DXVK DLLs; load it from there, not by name (another dxcompiler.dll,
		// without SPIR-V support, may already be in the process).
		wchar_t dxvkPath[MAX_PATH]{};
		std::filesystem::path compilerDirectory;
		if (::GetModuleFileNameW(d3d11, dxvkPath, MAX_PATH))
			compilerDirectory = std::filesystem::path(dxvkPath).parent_path();
		impl->shaderCompiler = std::make_unique<org::services::ShaderCompiler>(std::filesystem::path(L"Data/ShaderCache/ORG"), compilerDirectory);
		logger::info("[ORG] Runtime SPIR-V compilation {}", impl->shaderCompiler->Available() ? "available" : "unavailable (no dxcompiler.dll beside the DXVK DLLs)");
		if (!impl->shaderCompiler->Available())
			impl->shaderCompiler.reset();
	}
#endif
	IndirectCommandsFeatureInfo indirect{};
	if (impl->device->QueryFeatureInfo(&indirect.header) == rhi::Result::Ok) {
		logger::info("[ORG] Indirect commands: generated commands {}, index buffer arguments {}, pipeline sets {} (up to {} pipelines); game resource export {}",
			indirect.constantArguments, indirect.indexBufferArguments, indirect.pipelineSets, indirect.maxPipelineSetCount,
			impl->getResourceInfo ? "available" : "missing");
	}
	return true;
}

void RenderGraphRuntime::Shutdown()
{
	if (!impl)
		return;
	auto state = std::move(impl);
	if (state->setTeardownCallback)
		state->setTeardownCallback(nullptr, nullptr);
	// Graph work still in DXVK's stream must reach the queue before the host waits on its
	// timelines. If DXVK dropped it (its context is already gone), waiting would never end:
	// leak the graph instead, as at process exit.
	if (state->enqueueSubmission && !state->WaitForStreamSubmissions(std::chrono::seconds(5))) {
		logger::warn("[ORG] Queued render graph submissions never reached the queue; leaking the graph");
		if (state->device)
			rhi::vulkan::abandon_device(state->device.Get());
		(void)state->host.release();
		(void)state.release();
		disabledReason = "shut down";
		return;
	}
	try {
		// Waits only for the graph's own timelines, then releases every graph object.
		state->host.reset();
	} catch (const std::exception& e) {
		logger::error("[ORG] Render graph shutdown failed: {}", e.what());
	}
	// Non-owning: BasicRHI never destroys the adopted VkDevice.
	state->device.Reset();
	disabledReason = "shut down";
}

RenderGraphRuntime::~RenderGraphRuntime()
{
	// Static destruction at process exit: DXVK may already be gone, so no Vulkan
	// call is safe. Leak the graph state instead of tearing it down.
	if (impl) {
		if (impl->device)
			rhi::vulkan::abandon_device(impl->device.Get());
		(void)impl->host.release();
		(void)impl.release();
	}
}

bool RenderGraphRuntime::IsActive() const
{
	return impl && impl->host && !impl->faulted;
}

const std::string& RenderGraphRuntime::GetDisabledReason() const
{
	return disabledReason;
}

org::PersistentGraphHost* RenderGraphRuntime::Host()
{
	return IsActive() ? impl->host.get() : nullptr;
}

bool RenderGraphRuntime::ExecuteEpoch(Segment a_segment, const std::function<void(org::RenderGraph&)>& a_beforePrepare)
{
	if (!IsActive())
		return false;
	// Read by passes while the frame prepares and records, all before ExecuteFrame returns.
	segment = a_segment;
	// Where the epoch's submissions land in the D3D11 stream; the submissions themselves carry a queue
	// label of the same name (Submit).
	if (globals::state && globals::state->debuggerEvents)
		globals::state->SetPerfMarker(Impl::SegmentLabel(a_segment));
	const auto start = std::chrono::steady_clock::now();
	if (impl->enqueueSubmission) {
		// The graph's batches go into DXVK's command stream at this point, so they land between
		// the D3D11 work issued before and after; the entry/exit barriers order the memory.
		impl->streamThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
	} else {
		// Older DXVK (or CS_ORG_SUBMIT=flush): submit and wait for every D3D11 command recorded
		// so far, then submit the graph directly under DXVK's queue lock.
		impl->interop->FlushRenderingCommands();
	}
	impl->frameSegments[impl->host->FramesExecuted() % Impl::kSegmentRing] = a_segment;
	try {
		impl->host->ExecuteFrame(nullptr, a_beforePrepare);
		if (impl->epochStats)
			impl->RecordEpoch(std::chrono::steady_clock::now() - start);
		return true;
	} catch (const std::exception& e) {
		// A failed graph must not take the game down: callers fall back to D3D11.
		logger::error("[ORG] Render graph epoch failed, disabling the graph: {}", e.what());
		impl->faulted = true;
		disabledReason = e.what();
		return false;
	}
}

void RenderGraphRuntime::ReportGpuTimings(std::uint32_t a_frames, bool a_log)
{
	if (!impl || !a_frames)
		return;
	const double frames = double(a_frames);
	std::string summary;
	if (a_log)
		logger::info("[ORG] GPU time from ORG's pass timestamps, ms per frame over {} frames ({} epochs read back, {} completed):", a_frames, impl->timedEpochs, impl->completedFrames);
	for (std::size_t s = 0; s < Impl::kSegments; ++s) {
		auto& segmentTime = impl->segmentTimes[s];
		if (!segmentTime.epochs)
			continue;
		const char* label = Impl::SegmentLabel(static_cast<Segment>(s));
		std::vector<std::pair<std::string, Impl::PassTime>> passes(segmentTime.passes.begin(), segmentTime.passes.end());
		std::sort(passes.begin(), passes.end(), [](const auto& a, const auto& b) { return a.second.exclusiveMs > b.second.exclusiveMs; });
		double exclusiveTotal = 0.0;
		std::string detail;
		for (const auto& [name, time] : passes) {
			exclusiveTotal += time.exclusiveMs;
			// Every epoch runs every pass; the ones with nothing to do in this segment cost ~nothing.
			if (time.exclusiveMs / frames >= 0.005)
				detail += fmt::format("{}{} {:.3f} ({:.3f} incl)", detail.empty() ? "" : ", ", name, time.exclusiveMs / frames, time.inclusiveMs / frames);
		}
		if (a_log)
			logger::info("[ORG]   {}: {:.3f} ms span, {:.3f} ms in passes, {:.1f} epochs/frame; by pass (exclusive): {}",
				label, segmentTime.spanMs / frames, exclusiveTotal / frames, segmentTime.epochs / frames, detail.empty() ? "-" : detail);
		summary += fmt::format("{}{}: {:.3f} ms/frame ({:.1f} epochs/frame)", summary.empty() ? "" : "\n", label, segmentTime.spanMs / frames, segmentTime.epochs / frames);
		segmentTime = {};
	}
	impl->timedEpochs = 0;
	impl->completedFrames = 0;
	gpuTimingSummary = std::move(summary);
}

org::services::ShaderCompiler* RenderGraphRuntime::ShaderCompiler()
{
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	return impl ? impl->shaderCompiler.get() : nullptr;
#else
	return nullptr;
#endif
}

bool RenderGraphRuntime::DescribeResource(IUnknown* a_object, DxvkOrgInteropResourceInfo& a_info)
{
	if (!impl || !impl->getResourceInfo || !a_object)
		return false;
	a_info = {};
	a_info.version = DXVK_ORG_INTEROP_VERSION;
	return SUCCEEDED(impl->getResourceInfo(globals::d3d::device, a_object, &a_info));
}

winrt::com_ptr<ID3D11Buffer> RenderGraphRuntime::WrapBuffer(org::Resource& a_buffer, const D3D11_BUFFER_DESC& a_desc)
{
	winrt::com_ptr<ID3D11Buffer> result;
	if (!IsActive())
		return result;
	rhi::VulkanResourceInfo info{};
	const auto api = a_buffer.GetAPIResource();
	if (!rhi::vulkan::get_resource_info(api, info) || !info.resource) {
		logger::error("[ORG] WrapBuffer: '{}' has no Vulkan buffer", a_buffer.GetName());
		return result;
	}
	const auto buffer = rhi::vulkan::from_native_void<VkBuffer>(info.resource);
	if (FAILED(impl->createBufferFromVkBuffer(globals::d3d::device, &a_desc, buffer, result.put()))) {
		logger::error("[ORG] WrapBuffer: DXVK rejected '{}'", a_buffer.GetName());
		result = nullptr;
	}
	return result;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph (CS_ORG_ROOT missing): the runtime never activates
// and every feature keeps its D3D11 path.
#	include "RenderGraphRuntime.h"

struct RenderGraphRuntime::Impl
{};

RenderGraphRuntime& RenderGraphRuntime::Get()
{
	static RenderGraphRuntime s_runtime;
	return s_runtime;
}

void RenderGraphRuntime::RequestDeviceFeatures(HMODULE) {}

bool RenderGraphRuntime::Initialize()
{
	disabledReason = "built without OpenRenderGraph";
	return false;
}

void RenderGraphRuntime::Shutdown() {}
RenderGraphRuntime::~RenderGraphRuntime() = default;
bool RenderGraphRuntime::IsActive() const { return false; }
const std::string& RenderGraphRuntime::GetDisabledReason() const { return disabledReason; }
org::PersistentGraphHost* RenderGraphRuntime::Host() { return nullptr; }
bool RenderGraphRuntime::ExecuteEpoch(Segment, const std::function<void(org::RenderGraph&)>&) { return false; }
bool RenderGraphRuntime::DescribeResource(IUnknown*, DxvkOrgInteropResourceInfo&) { return false; }
org::services::ShaderCompiler* RenderGraphRuntime::ShaderCompiler() { return nullptr; }
winrt::com_ptr<ID3D11Buffer> RenderGraphRuntime::WrapBuffer(org::Resource&, const D3D11_BUFFER_DESC&) { return nullptr; }
void RenderGraphRuntime::ReportGpuTimings(std::uint32_t, bool) {}
#endif

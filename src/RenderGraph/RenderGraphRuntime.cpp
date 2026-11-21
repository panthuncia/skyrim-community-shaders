// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraphRuntime.h"

#include "Aftermath.h"
#include "DxvkExports.h"
#include "DxvkLoader.h"
#include "Features/Upscaling/DXVKInteropInterfaces.h"
#include "Globals.h"
#include "Profiler.h"
#include "RenderGraph/Backend.h"
#include "RenderGraph/BufferRefs.h"
#include "RenderGraph/BuildManager.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/ExternalWork.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/Frame/PostChain.h"
#include "RenderGraph/Frame/Recipe.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/Frame/Transients.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/DxvkImageImport.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Targets/EngineTargets.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/GpuTimingSource.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/UndeclaredAccess.h"
#include "RenderGraph/Switches.h"
#include "Features/RemoteControl/GpuTimestamps.h"
#include "Features/RemoteControl/GpuCounters.h"
#include "State.h"
#include "Utils/SharedShaderStore.h"

#include <OpenRenderGraph/GraphHost.h>
#include <OpenRenderGraph/Runtime.h>
#include <Render/Runtime/IUploadService.h>
#include <Render/Runtime/OpenRenderGraphSettings.h>
#include "RenderGraph/Async/Lanes.h"
#include "RenderGraph/Async/Retirement.h"
#include "RenderGraph/PipelineVersions.h"
#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/PipelineRegistry.h>
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
#include <ORGModuleServices/ShaderCompiler.h>
#endif
#include <Render/RenderGraph/RhiExecution.h>
#include <Resources/Resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <filesystem>
#include <semaphore>
#include <unordered_map>

#include <tbb/concurrent_queue.h>

namespace
{
	// The loader BasicRHI's volk loaded. CS does not use volk's globals.
	PFN_vkGetInstanceProcAddr LoaderProcAddr()
	{
		static const auto s_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(::GetProcAddress(::LoadLibraryW(L"vulkan-1.dll"), "vkGetInstanceProcAddr"));
		return s_proc;
	}
}
#include <map>
#include <mutex>
#include <chrono>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

namespace
{
	// RequestTimestampDump's: the completed epochs still to dump.
	std::atomic<int> timestampDumps{ 0 };
}
#include <fmt/ranges.h>

namespace
{
	// Features BasicRHI needs beyond DXVK's own. The graph uses descriptor buffers only, since VK_EXT_descriptor_heap rendered DLSS black on NVIDIA.
	// vulkanMemoryModelDeviceScope is needed because DXVK enables vulkanMemoryModel, which makes DXC's device-scope atomics invalid without it.
	constexpr DxvkOrgInteropFeature kRequestedFeatures[] = {
		{ VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, "descriptorBuffer" },
		{ VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME, "mutableDescriptorType" },
		// The descriptor-buffer model's heap sets are variable-count bindings (a bound heap need only cover its own capacity).
		{ nullptr, "descriptorBindingVariableDescriptorCount" },
		{ nullptr, "vulkanMemoryModelDeviceScope" },
		// FidelityFX's Vulkan backend barriers a depth-stencil image with the depth aspect alone. That is valid only with
		// separateDepthStencilLayouts, and AMD backs the game's D24S8 depth with D32_SFLOAT_S8_UINT (VUID-VkImageMemoryBarrier-image-03320).
		{ nullptr, "separateDepthStencilLayouts" },
		{ VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME, "deviceGeneratedCommands" },
	};

	// What DXVK takes from the device's descriptor budgets, for BasicRHI's limit checks.
	// DXVK keeps up to DxvkSamplerPool::MaxSamplerCount samplers alive, with a descriptor each.
	constexpr uint32_t kDxvkSamplerPoolSize = 2048;
	// DxvkResourceDescriptorHeap::addRanges uses 8 MiB per resource descriptor buffer on 64-bit. One buffer is the steady state.
	constexpr VkDeviceSize kDxvkResourceDescriptorBufferBytes = 8ull << 20;

	// DXVK's sampler descriptor buffer size. Estimated from samplerDescriptorSize, since the exact size needs a device.
	// 0 when the device lacks VK_EXT_descriptor_buffer.
	VkDeviceSize DxvkSamplerDescriptorBufferBytes(PFN_vkGetInstanceProcAddr a_getInstanceProcAddr, VkInstance a_instance,
		VkPhysicalDevice a_physicalDevice)
	{
		const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
			a_getInstanceProcAddr(a_instance, "vkEnumerateDeviceExtensionProperties"));
		const auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
			a_getInstanceProcAddr(a_instance, "vkGetPhysicalDeviceProperties2"));
		if (!enumerate || !getProperties2)
			return 0;
		uint32_t count = 0;
		if (enumerate(a_physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS)
			return 0;
		std::vector<VkExtensionProperties> extensions(count);
		if (enumerate(a_physicalDevice, nullptr, &count, extensions.data()) != VK_SUCCESS)
			return 0;
		// A structure of an extension the device lacks must not be chained into the query.
		const bool supported = std::any_of(extensions.begin(), extensions.begin() + count, [](const VkExtensionProperties& a_ext) {
			return std::strcmp(a_ext.extensionName, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME) == 0;
		});
		if (!supported)
			return 0;
		VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptorBuffer{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT };
		VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		properties.pNext = &descriptorBuffer;
		getProperties2(a_physicalDevice, &properties);
		const VkDeviceSize bytes = VkDeviceSize(kDxvkSamplerPoolSize) * descriptorBuffer.samplerDescriptorSize;
		const VkDeviceSize alignment = std::max<VkDeviceSize>(descriptorBuffer.descriptorBufferOffsetAlignment, 1);
		return (bytes + alignment - 1) / alignment * alignment;
	}

	// The binding requirements BasicRHI checks the device with. Negotiation and adoption must use the same ones.
	rhi::vulkan::BindingRequirements GraphBindingRequirements(PFN_vkGetInstanceProcAddr a_getInstanceProcAddr, VkInstance a_instance,
		VkPhysicalDevice a_physicalDevice)
	{
		rhi::vulkan::BindingRequirements requirements{};
		requirements.heapCapacities = rhi::DescriptorHeapCapacities{};
		// DXVK's sampler pool shares maxSamplerAllocationCount with the samplers BasicRHI creates on the descriptor-buffer model.
		requirements.hostSamplerObjects = kDxvkSamplerPoolSize;
		requirements.hostResourceDescriptorBufferBytes = kDxvkResourceDescriptorBufferBytes;
		requirements.hostSamplerDescriptorBufferBytes = DxvkSamplerDescriptorBufferBytes(a_getInstanceProcAddr, a_instance, a_physicalDevice);
		return requirements;
	}

	// Called inside DXVK's device creation, before the VkDevice exists. Must not call into D3D11 or DXVK, nor throw.
	uint32_t NegotiateBindingModel(void*, VkInstance a_instance, VkPhysicalDevice a_physicalDevice,
		PFN_vkGetInstanceProcAddr a_getInstanceProcAddr, uint32_t a_dxvkPreferredModel) noexcept
	{
		try {
			const rhi::vulkan::BindingRequirements requirements = GraphBindingRequirements(a_getInstanceProcAddr, a_instance, a_physicalDevice);
			const auto report = rhi::vulkan::EvaluateBindingModels(a_getInstanceProcAddr, a_instance, a_physicalDevice, requirements);
			const auto modelNames = [](uint32_t a_mask) {
				std::string names;
				if (a_mask & rhi::vulkan::BindingModel_DescriptorHeap)
					names += "descriptor heap";
				if (a_mask & rhi::vulkan::BindingModel_DescriptorBuffer)
					names += names.empty() ? "descriptor buffer" : ", descriptor buffer";
				return names.empty() ? std::string("none") : names;
			};
			logger::info("[ORG] Binding model negotiation: DXVK prefers {}, BasicRHI supports {}",
				a_dxvkPreferredModel ? modelNames(a_dxvkPreferredModel) : std::string("legacy descriptor sets"), modelNames(report.supportedModels));
			for (const auto& check : report.checks)
				logger::info("[ORG]   {}", check);
			logger::info("[ORG] Shader-visible heap capacities: descriptor heap {} / {}, descriptor buffer {} / {} (CBV/SRV/UAV / sampler; 0: unsupported)",
				report.heapModelResources, report.heapModelSamplers, report.bufferModelResources, report.bufferModelSamplers);
			// Descriptor buffers alone (kRequestedFeatures).
			return report.supportedModels & rhi::vulkan::BindingModel_DescriptorBuffer;
		} catch (...) {
			return 0;
		}
	}

	// A feature struct in the chain the device was created with (DxvkOrgInteropDeviceInfo::enabledFeatures), or null.
	template <class T>
	const T* FindFeatures(const VkPhysicalDeviceFeatures2* a_chain, VkStructureType a_type)
	{
		for (auto* s = a_chain ? static_cast<const VkBaseInStructure*>(a_chain->pNext) : nullptr; s; s = s->pNext)
			if (s->sType == a_type)
				return reinterpret_cast<const T*>(s);
		return nullptr;
	}

	// ConfigureFrame's segments (their values) as the epochs they run as.
	std::vector<std::uint32_t> EpochsOf(const std::vector<std::uint32_t>& a_segments)
	{
		std::vector<std::uint32_t> epochs;
		epochs.reserve(a_segments.size());
		for (const auto segment : a_segments)
			epochs.push_back(RenderGraphRuntime::EpochOf(static_cast<RenderGraphRuntime::Segment>(segment)));
		return epochs;
	}

	bool EnvDisabled()
	{
		return Switches::Off("CS_ORG");
	}

	// CS_ORG=features enables the interop device features without the graph, to separate their side effects.
	bool EnvFeaturesOnly()
	{
		return Switches::Equals("CS_ORG", "features");
	}

	/**
	* @brief A graph fault in graph mode ends the game. The engine's draws are withheld there and nothing else draws the frame.
	* Outside graph mode (CS_ORG=0 or features, native D3D11, VR) the graph stays off and the game runs on D3D11.
	*/
	void FaultIsFatal(std::string_view a_what)
	{
		if (!RenderGraph::Engine::GraphMode())
			return;
		RenderGraph::Fatal(fmt::format("the render graph in {}", RenderGraph::Engine::Native::Requested() ? "native mode" : "guest mode"),
			fmt::format("{}. Graph mode has no D3D11 path, so the game stops. Send CommunityShaders.log to the developers, or set CS_ORG=0 to play "
						"without the render graph.",
				a_what));
	}

	bool HasExtension(const DxvkOrgInteropDeviceInfo& a_info, const char* a_name)
	{
		for (uint32_t i = 0; i < a_info.enabledExtensionCount; ++i)
			if (a_info.enabledExtensions[i] && std::strcmp(a_info.enabledExtensions[i], a_name) == 0)
				return true;
		return false;
	}
}

namespace
{
	// Written once the device is described. Shader compiles read it on any thread.
	std::atomic<std::uint32_t> adoptedVendorId{ 0 };
}

std::uint32_t RenderGraphRuntime::VendorId()
{
	return adoptedVendorId.load(std::memory_order_relaxed);
}

struct RenderGraphRuntime::Impl
{
	winrt::com_ptr<IDXGIVkInteropDevice> interop;
	PFN_dxvkCreateBufferFromVkBuffer createBufferFromVkBuffer = nullptr;
	// Guest mode has every DXVK export: DxvkLoader stops graph mode without one. Native mode has none of them.
	PFN_dxvkCreateTextureFromVkImage createTextureFromVkImage = nullptr;
	PFN_dxvkCreateTextureFromVkImageWithData createTextureFromVkImageWithData = nullptr;
	// ReleaseAfterD3D11: owners held until an event query issued after their release completes, oldest first.
	struct D3D11Release
	{
		winrt::com_ptr<ID3D11Query> query;
		std::shared_ptr<const void> owner;
	};
	std::deque<D3D11Release> d3d11Releases;
	// Guest mode EmitCommands wrappers, released at frame end behind the frame's work.
	// A release query issued at the call would land inside a stage's window in DXVK's stream.
	std::mutex frameEndLock;
	std::vector<std::shared_ptr<const void>> frameEndReleases;
	// Native mode has no DXVK. An owner is freed once every graph submission made before its release completes.
	bool native = false;
	// Work on the graphics queue that the host does not number must also finish before the owner goes.
	struct NativeRelease
	{
		std::uint64_t submissions = 0;
		rhi::QueueProgress queue{};
		std::shared_ptr<const void> owner;
	};
	std::deque<NativeRelease> nativeReleases;
	std::atomic<std::uint64_t> retiredSubmissions{ 0 };
	PFN_dxvkSetDeviceTeardownCallback setTeardownCallback = nullptr;
	PFN_dxvkEnqueueInteropSubmission enqueueSubmission = nullptr;
	// An epoch's submissions go to DXVK as one enqueue.
	PFN_dxvkEnqueueInteropSubmissions enqueueSubmissions = nullptr;
	PFN_dxvkGetInteropResourceInfo getResourceInfo = nullptr;
	PFN_dxvkGetInteropResourceInfos getResourceInfos = nullptr;
	// Pinless imports.
	PFN_dxvkTrackInteropResources trackResources = nullptr;
	PFN_dxvkUntrackInteropResources untrackResources = nullptr;
	PFN_dxvkPollInteropRelocations pollRelocations = nullptr;
	PFN_dxvkReleaseInteropStorage releaseStorage = nullptr;
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
	std::unique_ptr<org::services::ShaderCompiler> shaderCompiler;
#endif
	PFN_vkQueueSubmit2 queueSubmit2 = nullptr;
	PFN_dxvkEmitExternalCommands emitExternalCommands = nullptr;
	PFN_vkGetDeviceProcAddr deviceProcAddr = nullptr;
	VkDevice adoptedDevice = VK_NULL_HANDLE;
	// DXVK's compute-only queue, used by the graph directly. Submissions still take DXVK's submission lock.
	// Compute and DXVK submissions made at the same time lost the device, with a page fault at address 0.
	VkQueue computeQueue = VK_NULL_HANDLE;
	// BasicRHI's copy queue. DXVK's spare transfer queue, else the shared upload queue. A blocked batch here can deadlock the GPU,
	// so each submission waits for its points on the CPU first. DXVK's own transfer queue cannot be shared at all.
	VkQueue copyQueue = VK_NULL_HANDLE;
	// DXVK's upload queue, offered to BasicRHI for ORG's upload service. It needs no order against DXVK's stream,
	// because it only writes resources that become current after its copies complete. Still under DXVK's submission lock.
	VkQueue uploadQueue = VK_NULL_HANDLE;
	// DXVK's stream signals streamPoint before an epoch's first compute or copy submission, and waits for those queues at its end.
	// That wait uses computeExit or copyExit, never graph timelines, because a rebuild destroys those while DXVK may still hold the wait.
	VkDevice vkDevice = VK_NULL_HANDLE;
	VkSemaphore streamPoint = VK_NULL_HANDLE;
	uint64_t streamPointValue = 0;
	VkSemaphore computeExit = VK_NULL_HANDLE;
	uint64_t computeExitValue = 0;
	VkSemaphore copyExit = VK_NULL_HANDLE;
	uint64_t copyExitValue = 0;
	bool streamPointEnqueued = false;  // this epoch's
	bool computeEntered = false;
	bool copyEntered = false;
	std::vector<VkSemaphoreSubmitInfo> computeTail;
	std::vector<VkSemaphoreSubmitInfo> computeWaits;  // scratch for the entry submission's waits

	// Open from ExecuteExternalEpoch until CloseExternalEpoch adds the stream's wait. Render thread.
	bool externalOpen = false;
	bool externalExitSplit = false;
	const char* externalLabel = "";

	// The stream-order guard. DXVK waits at the start of a submission and signals at its end, so waiting on off-stream work that
	// waits for the current list's signal would hang the GPU. The guard flushes first. Readers on other threads take streamOrderMutex.
	struct StreamPoint
	{
		VkSemaphore semaphore = VK_NULL_HANDLE;
		std::uint64_t value = 0;
	};
	// A semaphore DXVK's stream signals. open is the highest value signalled in DXVK's current command list. closed is the
	// highest value signalled in a list that has ended.
	struct StreamSignal
	{
		VkSemaphore semaphore = VK_NULL_HANDLE;
		std::uint64_t open = 0;
		std::uint64_t closed = 0;
	};
	// A compute or copy queue submission. dependencies holds, per semaphore, the highest value this submission or an earlier one
	// on its queue waits for. The queue runs its submissions in order, so a wait for this one is a wait for those too.
	struct OffStreamSubmission
	{
		std::uint64_t serial = 0;
		bool copy = false;
		std::vector<StreamPoint> signals;
		std::vector<StreamPoint> dependencies;
	};
	struct StreamEvent
	{
		std::uint64_t serial = 0;
		const char* kind = "";
		const char* label = "";
		std::uint64_t detail = 0;  // the off-stream submission's serial, or the segment's index
		std::uint8_t waits = 0;
		std::uint8_t signals = 0;
		bool truncated = false;
		std::array<StreamPoint, 10> points{};  // the waits first, then the signals
	};
	static constexpr std::size_t kOffStreamKept = 128;
	static constexpr std::size_t kStreamEvents = 512;
	mutable std::mutex streamOrderMutex;
	std::vector<StreamSignal> streamSignals;
	std::vector<VkSemaphore> offStreamSemaphores;          // every semaphore a compute or copy submission signalled
	std::deque<OffStreamSubmission> offStreamSubmissions;  // the latest kOffStreamKept of both queues, oldest first
	std::array<std::vector<StreamPoint>, 2> queueDependencies;  // the running maximum of each queue's waits: compute, copy
	std::uint64_t offStreamSerial = 0;
	std::unique_ptr<std::array<StreamEvent, kStreamEvents>> streamEvents = std::make_unique<std::array<StreamEvent, kStreamEvents>>();
	std::uint64_t streamEventCount = 0;
	std::uint64_t guardFlushes = 0;
	std::uint64_t guardUnordered = 0;
	std::uint32_t guardLogged = 0;

	static void RaiseTo(std::vector<StreamPoint>& a_points, VkSemaphore a_semaphore, std::uint64_t a_value)
	{
		const auto found = std::ranges::find(a_points, a_semaphore, &StreamPoint::semaphore);
		if (found == a_points.end())
			a_points.push_back({ a_semaphore, a_value });
		else
			found->value = (std::max)(found->value, a_value);
	}

	// The caller holds streamOrderMutex.
	template <class Wait, class Signal>
	void RecordStreamEventLocked(const char* a_kind, const char* a_label, std::uint64_t a_detail, std::span<const Wait> a_waits, std::span<const Signal> a_signals)
	{
		auto& event = (*streamEvents)[streamEventCount % kStreamEvents];
		event = {};
		event.serial = streamEventCount++;
		event.kind = a_kind;
		event.label = a_label ? a_label : "";
		event.detail = a_detail;
		std::size_t used = 0;
		for (const auto& wait : a_waits) {
			if (used == event.points.size()) {
				event.truncated = true;
				break;
			}
			event.points[used++] = { wait.semaphore, wait.value };
			++event.waits;
		}
		for (const auto& signal : a_signals) {
			if (used == event.points.size()) {
				event.truncated = true;
				break;
			}
			event.points[used++] = { signal.semaphore, signal.value };
			++event.signals;
		}
	}

	void RecordStreamEvent(const char* a_kind, const char* a_label, std::uint64_t a_detail = 0)
	{
		std::scoped_lock lock(streamOrderMutex);
		RecordStreamEventLocked<StreamPoint, StreamPoint>(a_kind, a_label, a_detail, {}, {});
	}

	// The caller holds streamOrderMutex.
	StreamSignal& StreamSignalLocked(VkSemaphore a_semaphore)
	{
		const auto found = std::ranges::find(streamSignals, a_semaphore, &StreamSignal::semaphore);
		if (found != streamSignals.end())
			return *found;
		return streamSignals.emplace_back(StreamSignal{ a_semaphore, 0, 0 });
	}

	// DXVK's current command list ended (a flush): every value signalled in it is closed.
	void NoteStreamClosed(const char* a_kind, const char* a_label)
	{
		std::scoped_lock lock(streamOrderMutex);
		for (auto& signal : streamSignals)
			signal.closed = (std::max)(signal.closed, signal.open);
		RecordStreamEventLocked<StreamPoint, StreamPoint>(a_kind, a_label, 0, {}, {});
	}

	// A submission enqueued in DXVK's stream. DXVK ended its command list first, and the submission is one of its own.
	void NoteEnqueued(const char* a_kind, const char* a_label, std::span<const VkSemaphoreSubmitInfo> a_waits, std::span<const VkSemaphoreSubmitInfo> a_signals)
	{
		std::scoped_lock lock(streamOrderMutex);
		for (auto& signal : streamSignals)
			signal.closed = (std::max)(signal.closed, signal.open);
		for (const auto& signal : a_signals) {
			auto& state = StreamSignalLocked(signal.semaphore);
			state.closed = (std::max)(state.closed, signal.value);
		}
		RecordStreamEventLocked(a_kind, a_label, 0, a_waits, a_signals);
	}

	// External commands joined DXVK's current command list. Their signals stay open until the list ends.
	void NoteExternalCommands(const char* a_label, std::span<const RenderGraphRuntime::ExternalCommandsWait> a_waits,
		std::span<const RenderGraphRuntime::ExternalCommandsWait> a_signals)
	{
		std::scoped_lock lock(streamOrderMutex);
		for (const auto& signal : a_signals) {
			auto& state = StreamSignalLocked(signal.semaphore);
			state.open = (std::max)(state.open, signal.value);
		}
		RecordStreamEventLocked(a_signals.empty() ? "wait-only commands" : "commands", a_label, 0, a_waits, a_signals);
	}

	// A compute or copy queue submission, as the hook hands it to its submitter.
	void NoteOffStream(bool a_copy, std::span<const VkSemaphoreSubmitInfo> a_waits, std::span<const VkSemaphoreSubmitInfo> a_signals)
	{
		std::scoped_lock lock(streamOrderMutex);
		auto& running = queueDependencies[a_copy ? 1 : 0];
		for (const auto& wait : a_waits)
			RaiseTo(running, wait.semaphore, wait.value);
		OffStreamSubmission submission;
		submission.serial = ++offStreamSerial;
		submission.copy = a_copy;
		for (const auto& signal : a_signals) {
			submission.signals.push_back({ signal.semaphore, signal.value });
			if (std::ranges::find(offStreamSemaphores, signal.semaphore) == offStreamSemaphores.end())
				offStreamSemaphores.push_back(signal.semaphore);
		}
		submission.dependencies = running;
		if (offStreamSubmissions.size() == kOffStreamKept)
			offStreamSubmissions.pop_front();
		offStreamSubmissions.push_back(std::move(submission));
		RecordStreamEventLocked(a_copy ? "copy submission" : "compute submission", "", offStreamSerial, a_waits, a_signals);
	}

	/** Render thread. Ends DXVK's command list when a wait would otherwise wait on its own list. Logs waits that can never be met. */
	void GuardStreamWaits(std::span<const RenderGraphRuntime::ExternalCommandsWait> a_waits, const char* a_label)
	{
		if (a_waits.empty())
			return;
		bool flush = false;
		std::uint64_t flushFor = 0;
		StreamPoint unordered{};
		std::uint64_t unorderedFor = 0;
		{
			std::scoped_lock lock(streamOrderMutex);
			if (offStreamSemaphores.empty())
				return;
			for (const auto& wait : a_waits) {
				if (std::ranges::find(offStreamSemaphores, wait.semaphore) == offStreamSemaphores.end())
					continue;
				// The wait ends with the first submission whose signal reaches its value.
				const OffStreamSubmission* awaited = nullptr;
				for (const auto& submission : offStreamSubmissions)
					if (std::ranges::any_of(submission.signals, [&](const StreamPoint& a_signal) { return a_signal.semaphore == wait.semaphore && a_signal.value >= wait.value; })) {
						awaited = &submission;
						break;
					}
				if (!awaited)
					continue;
				for (const auto& dependency : awaited->dependencies) {
					const auto signal = std::ranges::find(streamSignals, dependency.semaphore, &StreamSignal::semaphore);
					if (signal == streamSignals.end() || dependency.value <= signal->closed)
						continue;
					if (dependency.value <= signal->open) {
						flush = true;
						flushFor = awaited->serial;
					} else if (!unorderedFor) {
						unordered = dependency;
						unorderedFor = awaited->serial;
					}
				}
			}
		}
		if (flush) {
			globals::d3d::context->Flush();
			NoteStreamClosed("guard flush", a_label);
			std::uint32_t logged = 0;
			{
				std::scoped_lock lock(streamOrderMutex);
				++guardFlushes;
				if (guardLogged < 8)
					logged = ++guardLogged;
			}
			if (logged)
				logger::warn("[ORG] stream order: flushed DXVK's command list before {}'s wait. Off-stream submission #{} waits for a value that list "
							 "signals, so the list would have waited for itself. {}",
					a_label, flushFor, DescribeStreamOrder(32));
		}
		if (unorderedFor) {
			static const auto counterValue = deviceProcAddr ? reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(deviceProcAddr(adoptedDevice, "vkGetSemaphoreCounterValue")) : nullptr;
			std::uint64_t reached = 0;
			if (counterValue)
				(void)counterValue(vkDevice, unordered.semaphore, &reached);
			if (reached < unordered.value) {
				std::uint32_t logged = 0;
				{
					std::scoped_lock lock(streamOrderMutex);
					++guardUnordered;
					const StreamPoint point = unordered;
					RecordStreamEventLocked<StreamPoint, StreamPoint>("unordered wait", a_label, unorderedFor, { &point, 1 }, {});
					if (guardLogged < 8)
						logged = ++guardLogged;
				}
				if (logged)
					logger::error("[ORG] stream order: {} waits for off-stream submission #{}, which waits for {} {} (at {}). DXVK's stream has not "
								  "signalled that value, and this wait holds back what would: the GPU waits for itself. {}",
						a_label, unorderedFor, SemaphoreName(unordered.semaphore), unordered.value, reached, DescribeStreamOrder(48));
			}
		}
	}

	// A semaphore's name for the telemetry: the stream's own, the frame-wait timeline, a host timeline (t<index>), else its handle.
	std::string SemaphoreName(VkSemaphore a_semaphore) const
	{
		if (a_semaphore == streamPoint)
			return "streamPoint";
		if (a_semaphore == computeExit)
			return "computeExit";
		if (a_semaphore == copyExit)
			return "copyExit";
		if (a_semaphore == framePace)
			return "framePace";
		rhi::VulkanTimelineInfo info{};
		if (frameWaitTimeline && *frameWaitTimeline && rhi::QueryNativeTimeline(frameWaitTimeline->Get(), rhi::RHI_IID_VK_TIMELINE, &info, sizeof(info)) &&
			static_cast<VkSemaphore>(info.timeline) == a_semaphore)
			return "frameWait";
		if (host)
			for (const auto& [timeline, value] : host->PassedSealedPoint().points) {
				rhi::VulkanTimelineInfo hostInfo{};
				if (timeline && *timeline && rhi::QueryNativeTimeline(timeline->Get(), rhi::RHI_IID_VK_TIMELINE, &hostInfo, sizeof(hostInfo)) &&
					static_cast<VkSemaphore>(hostInfo.timeline) == a_semaphore)
					return fmt::format("t{}", (*timeline)->GetHandle().index);
			}
		return fmt::format("{}", static_cast<const void*>(a_semaphore));
	}

	std::string DescribeStreamOrder(std::size_t a_events) const
	{
		static const auto counterValue = deviceProcAddr ? reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(deviceProcAddr(adoptedDevice, "vkGetSemaphoreCounterValue")) : nullptr;
		std::vector<StreamEvent> events;
		std::vector<StreamSignal> signals;
		std::vector<OffStreamSubmission> submissions;
		std::uint64_t flushes = 0, unorderedWaits = 0;
		{
			std::scoped_lock lock(streamOrderMutex);
			const std::size_t count = (std::min)({ a_events, kStreamEvents, static_cast<std::size_t>(streamEventCount) });
			for (std::size_t i = count; i > 0; --i)
				events.push_back((*streamEvents)[(streamEventCount - i) % kStreamEvents]);
			signals = streamSignals;
			submissions.assign(offStreamSubmissions.begin(), offStreamSubmissions.end());
			flushes = guardFlushes;
			unorderedWaits = guardUnordered;
		}
		// Each semaphore's name and the value it has reached, looked up once.
		struct Named
		{
			VkSemaphore semaphore;
			std::string name;
			std::uint64_t reached;
		};
		std::deque<Named> names;
		const auto lookup = [&](VkSemaphore a_semaphore) -> const Named& {
			for (const auto& entry : names)
				if (entry.semaphore == a_semaphore)
					return entry;
			std::uint64_t reached = 0;
			if (counterValue && a_semaphore)
				(void)counterValue(vkDevice, a_semaphore, &reached);
			return names.emplace_back(Named{ a_semaphore, SemaphoreName(a_semaphore), reached });
		};
		std::string out = fmt::format("guard flushes {}, unordered waits {}; stream signals (open/closed/reached):", flushes, unorderedWaits);
		for (const auto& signal : signals) {
			const auto& entry = lookup(signal.semaphore);
			out += fmt::format(" {} {}/{}/{}", entry.name, signal.open, signal.closed, entry.reached);
		}
		// Only the off-stream submissions not done yet: their waits are the other side of a cycle.
		out += "; off-stream submissions not done:";
		for (const auto& submission : submissions) {
			bool done = !submission.signals.empty();
			for (const auto& signal : submission.signals)
				done = done && lookup(signal.semaphore).reached >= signal.value;
			if (done)
				continue;
			out += fmt::format("\n    #{} {} signals", submission.serial, submission.copy ? "copy" : "compute");
			for (const auto& signal : submission.signals)
				out += fmt::format(" {}={}", lookup(signal.semaphore).name, signal.value);
			out += "; waits (its own and the earlier ones' on its queue):";
			for (const auto& dependency : submission.dependencies) {
				const auto& entry = lookup(dependency.semaphore);
				out += fmt::format(" {}>={}", entry.name, dependency.value);
				if (entry.reached < dependency.value)
					out += fmt::format(" (at {}, PENDING)", entry.reached);
			}
		}
		out += fmt::format("; the last {} events, oldest first:", events.size());
		for (const auto& event : events) {
			out += fmt::format("\n    [{}] {}", event.serial, event.kind);
			if (event.label[0])
				out += fmt::format(" '{}'", event.label);
			if (event.detail)
				out += fmt::format(" #{}", event.detail);
			const std::size_t points = std::size_t{ event.waits } + event.signals;
			for (std::size_t i = 0; i < points; ++i) {
				const auto& point = event.points[i];
				const auto& entry = lookup(point.semaphore);
				if (i == 0 && event.waits)
					out += "; waits:";
				if (i == event.waits)
					out += "; signals:";
				out += fmt::format(" {}{}{}", entry.name, i < event.waits ? ">=" : "=", point.value);
				if (entry.reached < point.value)
					out += " (PENDING)";
			}
			if (event.truncated)
				out += " ...";
		}
		return out;
	}

	/** Each unreached point a shared copy queue submission waits for, and whether the seal covers it. */
	std::string DescribeUnsealed(std::span<const VkSemaphore> a_semaphores, std::span<const std::uint64_t> a_values)
	{
		static const auto counterValue = deviceProcAddr ? reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(deviceProcAddr(adoptedDevice, "vkGetSemaphoreCounterValue")) : nullptr;
		std::vector<std::pair<VkSemaphore, std::uint64_t>> sealed;
		std::vector<std::uint32_t> sealedIndex;  // each sealed timeline's BasicRHI index: t<index> in GraphHost::DescribeAsyncState
		if (host)
			for (const auto& [timeline, value] : host->PassedSealedPoint().points) {
				rhi::VulkanTimelineInfo info{};
				if (timeline && *timeline && rhi::QueryNativeTimeline(timeline->Get(), rhi::RHI_IID_VK_TIMELINE, &info, sizeof(info))) {
					sealed.emplace_back(static_cast<VkSemaphore>(info.timeline), value);
					sealedIndex.push_back((*timeline)->GetHandle().index);
				}
			}
		std::string out;
		for (std::size_t i = 0; i < a_semaphores.size(); ++i) {
			std::uint64_t reached = 0;
			if (counterValue)
				counterValue(vkDevice, a_semaphores[i], &reached);
			if (reached >= a_values[i])
				continue;
			const auto known = std::ranges::find(sealed, a_semaphores[i], &std::pair<VkSemaphore, std::uint64_t>::first);
			const char* state = known == sealed.end() ? "not in the seal" : a_values[i] > known->second ? "newer than the seal: a cycle if the frame waits for this queue" : "sealed";
			std::string name = a_semaphores[i] == streamPoint ? " streamPoint" : a_semaphores[i] == computeExit ? " computeExit" : a_semaphores[i] == copyExit ? " copyExit" : "";
			if (known != sealed.end())
				name += fmt::format(" t{}", sealedIndex[static_cast<std::size_t>(known - sealed.begin())]);
			out += fmt::format("{}{}{} wants {}, at {} ({}; sealed at {})", out.empty() ? "" : "; ", static_cast<const void*>(a_semaphores[i]), name, a_values[i], reached, state,
				known == sealed.end() ? 0 : known->second);
		}
		return out;
	}

	/**
	* Waits on the CPU until every point is reached. Bounded, so a stalled device is reported by the next submission.
	* a_rhiLocked means a BasicRHI submission holds the timeline lock, so telemetry must not read timelines through the RHI.
	*/
	void WaitForPoints(const VkSemaphoreSubmitInfo* a_points, std::uint32_t a_count, bool a_rhiLocked = false)
	{
		if (a_count == 0)
			return;
		ZoneScopedN("CS.ORG.WaitForPoints");
		std::vector<VkSemaphore> semaphores(a_count);
		std::vector<std::uint64_t> values(a_count);
		for (std::uint32_t i = 0; i < a_count; ++i) {
			semaphores[i] = a_points[i].semaphore;
			values[i] = a_points[i].value;
		}
		VkSemaphoreWaitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
		wait.semaphoreCount = a_count;
		wait.pSemaphores = semaphores.data();
		wait.pValues = values.data();
		// Half a second first. A point newer than the seal may only reach the device once the frame presents. The first few are named.
		if (waitSemaphores(vkDevice, &wait, 500'000'000ull) == VK_SUCCESS)
			return;
		static std::atomic<std::uint32_t> warned = 0;
		const bool named = warned.fetch_add(1, std::memory_order_relaxed) < 4;
		if (named) {
			logger::error("[ORG] A shared copy queue submission has waited 0.5 s for {}", DescribeUnsealed(semaphores, values));
			// What the host submitted and what each run waits for, the other side of the stall.
			logger::error("[ORG] DXVK stream at the copy queue's wait: {} graph submissions enqueued, {} submitted", streamEnqueued.load(std::memory_order_acquire),
				streamSubmitted.load(std::memory_order_acquire));
			if (host)
				logger::error("[ORG] host state at the copy queue's wait: {}", host->DescribeAsyncState(!a_rhiLocked));
			logger::error("[ORG] stream order at the copy queue's wait: {}", DescribeStreamOrder(64));
		}
		if (waitSemaphores(vkDevice, &wait, 9'500'000'000ull) != VK_SUCCESS) {
			logger::warn("[ORG] A shared copy queue submission went ahead after 10 s without its waits: {}", DescribeUnsealed(semaphores, values));
			if (named && host)
				logger::warn("[ORG] host state after 10 s: {}", host->DescribeAsyncState(!a_rhiLocked));
		}
	}

	/**
	* Submits a queue's batches on a thread of its own, since vkQueueSubmit2 costs the render thread microseconds per call.
	* Each submission takes DXVK's submission lock here, never on the render thread. A failure disables the graph at the next epoch.
	*/
	struct ComputeSubmitter
	{
		struct Batch
		{
			VkQueue queue = VK_NULL_HANDLE;
			std::vector<VkSemaphoreSubmitInfo> waits, signals;
			std::vector<VkCommandBufferSubmitInfo> commandBuffers;
			bool stop = false;
		};
		tbb::concurrent_queue<Batch> batches;
		std::counting_semaphore<> available{ 0 };
		std::atomic<std::uint64_t> pushed{ 0 }, submitted{ 0 };
		std::atomic<VkResult> failure{ VK_SUCCESS };
		std::thread thread;

		// a_waitOnCpu: each batch's waits are reached before it is submitted (a shared queue, see copyQueue).
		void Start(Impl& a_impl, const wchar_t* a_name, bool a_waitOnCpu)
		{
			thread = std::thread([this, &a_impl, a_name, a_waitOnCpu] {
				SetThreadDescription(GetCurrentThread(), a_name);
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
				for (;;) {
					available.acquire();
					Batch batch;
					if (!batches.try_pop(batch))
						continue;
					if (batch.stop)
						return;
					if (a_waitOnCpu)
						a_impl.WaitForPoints(batch.waits.data(), static_cast<std::uint32_t>(batch.waits.size()));
					VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
					submit.waitSemaphoreInfoCount = static_cast<uint32_t>(batch.waits.size());
					submit.pWaitSemaphoreInfos = batch.waits.data();
					submit.commandBufferInfoCount = static_cast<uint32_t>(batch.commandBuffers.size());
					submit.pCommandBufferInfos = batch.commandBuffers.data();
					submit.signalSemaphoreInfoCount = static_cast<uint32_t>(batch.signals.size());
					submit.pSignalSemaphoreInfos = batch.signals.data();
					VkResult result;
					{
						ZoneScopedN("CS.ORG.ComputeSubmit");
						// DXVK's submission lock (see computeQueue), taken here rather than on the render thread.
						a_impl.interop->LockSubmissionQueue();
						result = a_impl.queueSubmit2(batch.queue, 1, &submit, VK_NULL_HANDLE);
						a_impl.interop->ReleaseSubmissionQueue();
					}
					if (result != VK_SUCCESS) {
						auto expected = VK_SUCCESS;
						failure.compare_exchange_strong(expected, result);
					}
					submitted.fetch_add(1, std::memory_order_release);
					submitted.notify_all();
				}
			});
		}
		void Push(Batch&& a_batch)
		{
			pushed.fetch_add(1, std::memory_order_relaxed);
			batches.push(std::move(a_batch));
			available.release();
		}
		/** @brief Waits until every batch pushed so far is submitted (teardown). */
		void Flush()
		{
			const auto target = pushed.load(std::memory_order_relaxed);
			for (auto done = submitted.load(std::memory_order_acquire); done < target; done = submitted.load(std::memory_order_acquire))
				submitted.wait(done, std::memory_order_acquire);
		}
		~ComputeSubmitter() { Stop(); }
		void Stop()
		{
			if (!thread.joinable())
				return;
			Flush();
			Batch stop;
			stop.stop = true;
			batches.push(std::move(stop));
			available.release();
			thread.join();
		}
	};
	ComputeSubmitter computeSubmitter;
	ComputeSubmitter copySubmitter;
	PFN_vkDestroySemaphore destroySemaphore = nullptr;
	PFN_vkWaitSemaphores waitSemaphores = nullptr;
	// A timeline DXVK's stream signals with each paced frame's number. Lives as long as the device, since its last signal may still be queued.
	VkSemaphore framePace = VK_NULL_HANDLE;
	uint64_t framePaceValue = 0;  // the last value enqueued
	uint64_t framePaceWaits = 0, framePaceWaited = 0;  // waits that blocked, and their total time (us)
	uint32_t frameLatency = 2;    // CS_FRAME_LATENCY
	bool framePaceSignalled = false;  // this frame enqueued one
	std::vector<std::function<void()>> presentWaits;  // WaitBeforePresent: the next present's, render owner only
	std::shared_ptr<rhi::DevicePtr> device = std::make_shared<rhi::DevicePtr>();
	// ORG's runtime on the adopted device: outlives the host, goes before the device.
	std::shared_ptr<org::Runtime> orgRuntime;
	std::unique_ptr<org::GraphHost> host;
	// The host's structural builds, off the render thread. Destroyed before the host, so its thread stops and its graphs drop first.
	std::unique_ptr<RenderGraph::BuildManager> builds;
	std::shared_ptr<rhi::TimelinePtr> frameWaitTimeline;  // FrameWaitTimeline
	// Any device timeline, for NativeTimelinePoint. The frame-wait timeline cannot serve, since devices without a transfer family lack it.
	rhi::TimelinePtr nativeTimelines;
	std::atomic<bool> faulted = false;
	// Segments whose failed epoch was logged, a bit each. Cleared when the segment submits again. Render thread.
	std::uint64_t failedEpochsLogged = 0;
	std::uint64_t copiesBusyLogged = 0; // segments whose published copies were all busy, logged once each

	// The thread that drives the D3D11 immediate context (the one running epochs).
	// Its submissions go into DXVK's command stream; other threads submit directly.
	std::atomic<DWORD> streamThread = 0;
	// Submissions handed to DXVK's stream, and how many of those reached the queue.
	std::atomic<uint64_t> streamEnqueued = 0;
	std::atomic<uint64_t> streamSubmitted = 0;

	// RenderGraphRuntime::InStreamEpochs: the async epochs executed in DXVK's own command stream.
	std::vector<std::uint32_t> inStreamEpochs;
	std::uint64_t submittedSegments = 0;  // since TakeSubmittedSegments

	// With enqueueSubmissions, the hook's submissions are copied here and sent to DXVK together at the epoch's end.
	// The submit info is only valid during the call. Nothing waits on them on the CPU before then.
	struct PendingSubmit
	{
		std::vector<VkSemaphoreSubmitInfo> waits;
		std::vector<VkCommandBufferSubmitInfo> commandBuffers;
		std::vector<VkSemaphoreSubmitInfo> signals;
	};
	bool batching = false;
	std::atomic<uint64_t> offThreadSubmits = 0;
	std::vector<PendingSubmit> pendingSubmits;  // [0, pendingCount) in use; capacity kept across epochs
	std::size_t pendingCount = 0;
	std::vector<VkSubmitInfo2> pendingInfos;

	// Hands the epoch's collected submissions to DXVK as one enqueue. False when DXVK rejected them.
	bool FlushPendingSubmits(const char* a_label)
	{
		batching = false;
		if (!pendingCount)
			return true;
		pendingInfos.assign(pendingCount, VkSubmitInfo2{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 });
		for (std::size_t i = 0; i < pendingCount; ++i) {
			const auto& pending = pendingSubmits[i];
			auto& info = pendingInfos[i];
			info.waitSemaphoreInfoCount = static_cast<uint32_t>(pending.waits.size());
			info.pWaitSemaphoreInfos = pending.waits.data();
			info.commandBufferInfoCount = static_cast<uint32_t>(pending.commandBuffers.size());
			info.pCommandBufferInfos = pending.commandBuffers.data();
			info.signalSemaphoreInfoCount = static_cast<uint32_t>(pending.signals.size());
			info.pSignalSemaphoreInfos = pending.signals.data();
		}
		DxvkOrgInteropSubmissionBatch batch{};
		batch.version = DXVK_ORG_INTEROP_VERSION;
		batch.submitCount = static_cast<uint32_t>(pendingCount);
		batch.submits = pendingInfos.data();
		batch.onSubmitted = &Impl::OnStreamSubmitted;
		batch.user = this;
		batch.label = a_label;
		const std::size_t enqueued = pendingCount;
		pendingCount = 0;
		streamEnqueued.fetch_add(1, std::memory_order_acq_rel);
		if (FAILED(enqueueSubmissions(globals::d3d::device, &batch))) {
			streamEnqueued.fetch_sub(1, std::memory_order_acq_rel);
			return false;
		}
		for (std::size_t i = 0; i < enqueued; ++i)
			NoteEnqueued("graph submission", a_label, pendingSubmits[i].waits, pendingSubmits[i].signals);
		return true;
	}

	// ORG's pass timestamps per segment. Each run's segment is kept by frame number until its timestamps return.
	static constexpr std::size_t kSegments = static_cast<std::size_t>(Segment::Count);
	// Indexed by host frame number. The ring holds far more runs than are ever in flight.
	static constexpr std::size_t kSegmentRing = 2048;
	static_assert(kSegmentRing >= 4 * (RenderGraphRuntime::kRunCapacity + kSegments));
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
	// Written by the render thread, read when timestamps return, on the host's thread with async epochs.
	std::array<std::atomic<Segment>, kSegmentRing> frameSegments{};
	// The frame each host frame's epoch ran in while a timestamp trace collects. 0 when not collected.
	std::array<std::atomic<std::uint64_t>, kSegmentRing> frameTimestampFrames{};
	// Pacing measurement. Each host frame's begin frame (RenderGraph::FrameClock), and how far Begin ran ahead.
	std::array<std::atomic<std::uint64_t>, kSegmentRing> frameGameFrames{};
	std::array<std::uint64_t, 10> aheadCounts{};
	std::uint64_t aheadPeak = 0, aheadPeakSession = 0, aheadFrames = 0;
	std::array<SegmentTime, kSegments> segmentTimes{};
	// Per segment, each profiler name its timed epochs reported and how many of them since left it out. A segment can run
	// several epochs a frame with different passes (Interface, ImageSpace, EngineDraws), so a pass counts as stopped only
	// after kStoppedEpochs of them. It is then marked skipped, so its row reads 0 rather than its last time until it retires.
	static constexpr std::uint32_t kStoppedEpochs = 16;
	static constexpr std::uint32_t kForgottenEpochs = 256;
	std::array<std::unordered_map<std::string, std::uint32_t>, kSegments> profiledPasses{};
	std::uint32_t timedEpochs = 0;
	std::uint32_t completedFrames = 0;  // callbacks, timed or not: tells "no readback" from "no timestamps"

	// One epoch's pass timings. With async epochs the host's thread reads them and hands batches to the render thread
	// through completedInbox, publishing only once the previous batch was taken.
	struct CompletedPass
	{
		std::string name;
		double inclusiveMs = 0.0;
		double exclusiveMs = 0.0;
	};
	struct CompletedEpoch
	{
		std::size_t segment = kSegments;
		double spanMs = 0.0;
		bool timed = false;
		std::vector<CompletedPass> passes;
		// Passes on the compute queue: their ticks are that queue's, so they stay out of the graphics queue's exclusive times.
		std::vector<CompletedPass> otherQueuePasses;
		// While a timestamp trace collects: the frame the epoch ran in, and every range it timed (passes and barrier batches).
		std::uint64_t timestampFrame = 0;
		std::vector<GpuCounters::Timestamps::Range> ranges;
	};
	std::vector<CompletedEpoch> hostPending;  // the host's thread only
	std::atomic<std::shared_ptr<std::vector<CompletedEpoch>>> completedInbox;

	void Aggregate(CompletedEpoch& a_epoch)
	{
		++completedFrames;
		if (a_epoch.timestampFrame)
			GpuCounters::Timestamps::Completed(a_epoch.timestampFrame, std::move(a_epoch.ranges));
		if (a_epoch.segment >= kSegments)
			return;
		const auto epochSegment = static_cast<Segment>(a_epoch.segment);
		std::vector<std::string> reported;
		// Leave out passes with nothing to do, which cost a few microseconds of empty timestamps.
		const auto report = [&](const std::string& a_pass, double a_ms) {
			if (!globals::profiler || a_ms < kProfilerMinPassMs)
				return;
			auto name = ProfilerName(epochSegment, a_pass);
			globals::profiler->AddExternalSample(name, static_cast<float>(a_ms));
			reported.push_back(std::move(name));
		};
		for (const auto& completed : a_epoch.otherQueuePasses)
			report(completed.name + " [compute]", completed.inclusiveMs);
		if (!a_epoch.timed)
			return;
		auto& segmentTime = segmentTimes[a_epoch.segment];
		for (const auto& completed : a_epoch.passes) {
			auto& pass = segmentTime.passes[completed.name];
			pass.inclusiveMs += completed.inclusiveMs;
			pass.exclusiveMs += completed.exclusiveMs;
			report(completed.name, completed.exclusiveMs);
		}
		segmentTime.spanMs += a_epoch.spanMs;
		++segmentTime.epochs;
		++timedEpochs;
		if (globals::profiler) {
			auto& absent = profiledPasses[a_epoch.segment];
			for (auto& [name, epochs] : absent)
				if (++epochs > kStoppedEpochs)
					globals::profiler->MarkPassSkipped(name);
			std::erase_if(absent, [](const auto& a_entry) { return a_entry.second > kForgottenEpochs; });
			for (auto& name : reported)
				absent.insert_or_assign(std::move(name), 0u);
		}
	}

	// Render thread: the timings the host's thread has read back since the last call.
	void TakeCompletedEpochs()
	{
		if (auto batch = completedInbox.exchange(nullptr, std::memory_order_acq_rel))
			for (auto& epoch : *batch)
				Aggregate(epoch);
	}

	void OnCompletedFrame(std::uint64_t a_frameNumber, const org::runtime::IStatisticsService& a_stats)
	{
		GpuTimingSource::OnCompletedFrame(a_frameNumber, a_stats);
		// The frame clock's gpuCompleted: the begin frame of the epoch that completed.
		RenderGraphRuntime::NoteGpuCompleted(frameGameFrames[a_frameNumber % kSegmentRing].load(std::memory_order_acquire));
		CompletedEpoch epoch;
		ReadCompletedEpoch(a_frameNumber, a_stats, epoch);
		hostPending.push_back(std::move(epoch));
		std::shared_ptr<std::vector<CompletedEpoch>> expected;
		auto batch = std::make_shared<std::vector<CompletedEpoch>>(std::move(hostPending));
		if (!completedInbox.compare_exchange_strong(expected, batch, std::memory_order_acq_rel))
			hostPending = std::move(*batch);  // not taken yet: publish with the next one
		else
			hostPending.clear();
	}

	void ReadCompletedEpoch(std::uint64_t a_frameNumber, const org::runtime::IStatisticsService& a_stats, CompletedEpoch& a_out)
	{
		const auto segmentIndex = static_cast<std::size_t>(frameSegments[a_frameNumber % kSegmentRing].load(std::memory_order_acquire));
		a_out.segment = segmentIndex;
		a_out.timestampFrame = frameTimestampFrames[a_frameNumber % kSegmentRing].exchange(0, std::memory_order_acq_rel);
		// Every timestamp pair the run's completion read: the graph's per-frame passes (the shared pool, graphics queue)
		// and the recordings kept across frames (their own queries, every queue, and their barrier batches).
		const auto& timestamps = a_stats.GetCompletedTimestamps();
		const double toMs = a_stats.GetGpuTicksToMilliseconds();
		static std::uint32_t described = 0;
		if (described < 3 || (a_out.timestampFrame && described < 6)) {
			++described;
			std::size_t replayed = 0, otherQueue = 0, barriers = 0;
			for (const auto& timestamp : timestamps) {
				replayed += timestamp.replayed;
				otherQueue += timestamp.queueKind != rhi::QueueKind::Graphics;
				barriers += timestamp.kind == org::runtime::TimestampRangeKind::Barrier;
			}
			logger::info("[ORG] completed frame {}: {} timestamp pairs read back ({} from kept recordings, {} on other queues, {} barrier batches), {:.6f} ms per tick",
				a_frameNumber, timestamps.size(), replayed, otherQueue, barriers, toMs);
		}
		// RequestTimestampDump logs each graphics range of every epoch in GPU order.
		if (timestampDumps.load(std::memory_order_relaxed) > 0 && timestampDumps.fetch_sub(1, std::memory_order_relaxed) > 0) {
			std::vector<const org::runtime::CompletedTimestamp*> sorted;
			for (const auto& timestamp : timestamps)
				if (timestamp.queueKind == rhi::QueueKind::Graphics && timestamp.beginTick)
					sorted.push_back(&timestamp);
			std::ranges::sort(sorted, [](auto* a, auto* b) { return a->beginTick < b->beginTick || (a->beginTick == b->beginTick && a->endTick < b->endTick); });
			const std::uint64_t origin = sorted.empty() ? 0 : sorted.front()->beginTick;
			logger::info("[TSDUMP] frame {} ({}): {} graphics ranges of {} timestamp pairs", a_frameNumber,
				segmentIndex < kSegments ? SegmentLabel(static_cast<Segment>(segmentIndex)) : "no fragment source's", sorted.size(), timestamps.size());
			for (const auto* t : sorted)
				logger::info("[TSDUMP] {:10.2f} {:10.2f} {:8.2f} k{} r{} o{} {}", double(t->beginTick - origin) * toMs * 1000.0,
					double(t->endTick - origin) * toMs * 1000.0, double(t->endTick - t->beginTick) * toMs * 1000.0, static_cast<int>(t->kind),
					t->replayed ? 1 : 0, t->occurrence, t->name);
		}
		if (segmentIndex >= kSegments)
			return;
		if (a_out.timestampFrame) {
			a_out.ranges.reserve(timestamps.size());
			for (const auto& timestamp : timestamps) {
				GpuCounters::Timestamps::Range range;
				range.name = std::string(timestamp.name);
				range.queue = timestamp.queueKind == rhi::QueueKind::Graphics ? GpuCounters::Timestamps::Queue::Graphics :
				              timestamp.queueKind == rhi::QueueKind::Compute  ? GpuCounters::Timestamps::Queue::Compute :
				                                                                GpuCounters::Timestamps::Queue::Copy;
				range.barrier = timestamp.kind == org::runtime::TimestampRangeKind::Barrier;
				range.nested = timestamp.kind == org::runtime::TimestampRangeKind::Sub;
				range.replayed = timestamp.replayed;
				range.us = timestamp.milliseconds * 1000.0;
				range.beginTick = timestamp.beginTick;
				range.endTick = timestamp.endTick;
				range.usPerTick = toMs * 1000.0;
				a_out.ranges.push_back(std::move(range));
			}
		}
		struct Sample
		{
			std::string_view name;
			std::uint64_t begin;
			std::uint64_t end;
		};
		std::vector<Sample> samples;
		for (const auto& timestamp : timestamps) {
			if (timestamp.kind != org::runtime::TimestampRangeKind::Pass || !timestamp.beginTick || timestamp.endTick < timestamp.beginTick)
				continue;
			if (timestamp.queueKind == rhi::QueueKind::Graphics)
				samples.push_back({ timestamp.name, timestamp.beginTick, timestamp.endTick });
			else
				a_out.otherQueuePasses.push_back({ std::string(timestamp.name), timestamp.milliseconds, timestamp.milliseconds });
		}
		if (samples.empty() || toMs <= 0.0)
			return;
		std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) { return a.begin < b.begin; });
		std::uint64_t previousEnd = samples.front().begin;
		for (const auto& sample : samples) {
			CompletedPass pass{ std::string(sample.name) };
			pass.inclusiveMs = double(sample.end - sample.begin) * toMs;
			const std::uint64_t from = (std::max)(previousEnd, sample.begin);
			pass.exclusiveMs = sample.end > from ? double(sample.end - from) * toMs : 0.0;
			previousEnd = (std::max)(previousEnd, sample.end);
			a_out.passes.push_back(std::move(pass));
		}
		a_out.spanMs = double(previousEnd - samples.front().begin) * toMs;
		a_out.timed = true;
	}

	static constexpr double kProfilerMinPassMs = 0.003;

	/** @brief The feature whose client made a pass, from the client id the pass name starts with. Null for the engine's and CS's own. */
	static const char* PassFeature(std::string_view a_pass)
	{
		static constexpr std::pair<std::string_view, const char*> kClients[] = {
			{ "cs.pp", "PostProcessing" },
			{ "cs.upscaling", "Upscaling" },
			{ "cs.screenshot", "Screenshot" },
			{ "cs.hdr", "HDRDisplay" },
			{ "cs.effects11", "Effects11" },
			{ "cs.e11-rays", "Effects11" },
			{ "cs.volumetric-lighting", "VolumetricLighting" },
			{ "cs.volumetric-shadows", "VolumetricShadows" },
			{ "cs.llf", "LightLimitFix" },
			{ "cs.dyncubemaps", "DynamicCubemaps" },
			{ "cs.subsurface", "SubsurfaceScattering" },
			{ "cs.skylighting", "Skylighting" },
			{ "cs.exp-height-fog", "ExponentialHeightFog" },
			{ "cs.view-atmosphere", "ViewAtmosphere" },
			{ "cs.ssgi", "ScreenSpaceGI" },
			{ "cs.screen-space-shadows", "ScreenSpaceShadows" },
			{ "cs.cloud-shadows", "CloudShadows" },
			{ "cs.terrain-blending", "TerrainBlending" },
			{ "cs.terrain-shadows", "TerrainShadows" },
			{ "cs.ibl", "ImageBasedLighting" },
			{ "cs.physical-sky", "PhysicalSky" },
			{ "cs.grass-collision", "GrassCollision" },
			{ "cs.texstream", "TextureStreaming" },
			{ "cs.dclf", "DrawcallLimitFix" },
			{ "cs.water", "DrawcallLimitFix" },
			{ "cs.precipitation", "DrawcallLimitFix" },
			{ "cs.decals", "DrawcallLimitFix" },
			{ "cs.blood-splatter", "DrawcallLimitFix" },
			{ "cs.shadow-mask", "DrawcallLimitFix" },
			{ "cs.sky", "DrawcallLimitFix" },
			{ "cs.menu-scenes", "DrawcallLimitFix" },
		};
		for (const auto& [client, feature] : kClients)
			if (a_pass.starts_with(client) && (a_pass.size() == client.size() || a_pass[client.size()] == '.'))
				return feature;
		return nullptr;
	}

	// "<Feature>::<segment> / <pass>" so the profiler groups passes by feature, and the menu's feature cost chip finds them. An
	// anchor's or stage's pass goes under the feature its client belongs to. These are an epoch's only timings, since a D3D11
	// timer around ExecuteEpoch would also count queue idle time.
	static std::string ProfilerName(Segment a_segment, const std::string& a_pass)
	{
		switch (a_segment) {
		case Segment::ZPrepass:
			return "DrawcallLimitFix::Z-prepass / " + a_pass;
		case Segment::MainOpaque:
			return "DrawcallLimitFix::Main opaque / " + a_pass;
		case Segment::ShadowView:
			return "DrawcallLimitFix::Shadow views / " + a_pass;
		case Segment::SkyOcclusion:
			return "DrawcallLimitFix::Skylighting occlusion / " + a_pass;
		case Segment::Reflection:
			return "DrawcallLimitFix::Reflection faces / " + a_pass;
		case Segment::Forward:
			return "DrawcallLimitFix::Forward / " + a_pass;
		case Segment::FirstPersonDepth:
			return "DrawcallLimitFix::First-person depth / " + a_pass;
		case Segment::FirstPerson:
			return "DrawcallLimitFix::First person / " + a_pass;
		case Segment::WaterMask:
			return "DrawcallLimitFix::Water mask / " + a_pass;
		case Segment::WaterSurface:
			return "DrawcallLimitFix::Water surface / " + a_pass;
		default:
			if (const auto* anchor = FrameAnchors::NamesOf(static_cast<std::uint32_t>(a_segment))) {
				// "FrameAnchors::Tonemap / " becomes "PostProcessing::Tonemap / " for a Post Processing pass.
				const std::string_view profiler = anchor->profiler;
				const auto separator = profiler.find("::");
				if (const char* feature = PassFeature(a_pass); feature && separator != std::string_view::npos)
					return std::format("{}::{}{}", feature, profiler.substr(separator + 2), a_pass);
				return anchor->profiler + a_pass;
			}
			break;
		}
		return "RenderGraph::" + a_pass;
	}

	// Render thread, for the epoch a host frame runs: its segment, and while a timestamp trace collects, the frame it runs in.
	void MarkEpoch(std::uint64_t a_hostFrame, Segment a_segment)
	{
		const std::uint64_t timestampFrame = GpuCounters::Timestamps::Collecting() ? GpuCounters::Timestamps::CurrentFrame() + 1 : 0;
		frameTimestampFrames[a_hostFrame % kSegmentRing].store(timestampFrame, std::memory_order_release);
		frameSegments[a_hostFrame % kSegmentRing].store(a_segment, std::memory_order_release);
		frameGameFrames[a_hostFrame % kSegmentRing].store(RenderGraph::FrameClock::BeginFrame(), std::memory_order_release);
		if (timestampFrame)
			GpuCounters::Timestamps::Submitted(timestampFrame);
		GpuTimingSource::OnEpochSubmitted(a_hostFrame, SegmentLabel(a_segment));
	}

	// DXVK's stream signals streamPoint here, after every D3D11 command so far and ahead of the epoch's graph work.
	bool EnqueueStreamPoint()
	{
		const VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, streamPoint, streamPointValue + 1,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
		DxvkOrgInteropSubmission submission{};
		submission.version = DXVK_ORG_INTEROP_VERSION;
		submission.signalCount = 1;
		submission.signals = &signal;
		submission.onSubmitted = &Impl::OnStreamSubmitted;
		submission.user = this;
		submission.label = "CS ORG: compute entry";
		streamEnqueued.fetch_add(1, std::memory_order_acq_rel);
		if (FAILED(enqueueSubmission(globals::d3d::device, &submission))) {
			streamEnqueued.fetch_sub(1, std::memory_order_acq_rel);
			return false;
		}
		++streamPointValue;
		NoteEnqueued("stream point", submission.label, {}, { &signal, 1 });
		return true;
	}

	static void NoCommands(void*, VkCommandBuffer) {}

	// One segment of an in-stream epoch's execution, for its callback on DXVK's CS thread (which frees it).
	struct StreamSegment
	{
		std::shared_ptr<org::ExternalEpochExecution> execution;
		std::size_t index = 0;
		rhi::Device device;
	};
	static void StreamSegmentCallback(void* a_user, VkCommandBuffer a_commandBuffer)
	{
		std::unique_ptr<StreamSegment> segment(static_cast<StreamSegment*>(a_user));
		ZoneScopedN("CS.ORG.StreamSegment");
		// Later segments run only after a failed one, so they are left undone too.
		if (segment->execution->ExecutedSegments() != segment->index) {
			static std::atomic_flag logged;
			if (!logged.test_and_set())
				logger::error("[ORG] an in-stream epoch's segment ran out of order: it and the ones after it draw nothing");
			return;
		}
		auto adopted = rhi::vulkan::AdoptCommandBuffer(segment->device, a_commandBuffer, rhi::QueueKind::Graphics);
		if (!adopted) {
			static std::atomic_flag logged;
			if (!logged.test_and_set())
				logger::error("[ORG] DXVK's command buffer could not be adopted: an in-stream epoch's segment draws nothing");
			return;
		}
		try {
			segment->execution->Execute(segment->index, adopted.Get());
		} catch (const std::exception& e) {
			static std::atomic_flag logged;
			if (!logged.test_and_set())
				logger::error("[ORG] an in-stream epoch's segment failed: {}", e.what());
		}
	}
	// Render thread. Emits an in-stream execution into DXVK's stream, one external command per segment.
	// DXVK's list is flushed before a split segment. If not taken, the rest is signalled unexecuted.
	bool EmitExecution(const std::shared_ptr<org::ExternalEpochExecution>& a_execution, const char* a_label)
	{
		ZoneScopedN("CS.ORG.EmitExecution");
		thread_local std::vector<RenderGraphRuntime::ExternalCommandsWait> waits, signals;
		const auto streamDevice = host->Device();
		for (std::size_t index = 0; index < a_execution->segments.size(); ++index) {
			const auto& streamSegment = a_execution->segments[index];
			if (streamSegment.split && index) {
				globals::d3d::context->Flush();
				NoteStreamClosed("segment split", a_label);
			}
			waits.clear();
			signals.clear();
			for (const auto& point : streamSegment.waits)
				if (const auto converted = RenderGraphRuntime::NativeTimelinePoint(point); converted.semaphore)
					waits.push_back(converted);
			bool resolved = true;
			for (const auto& point : streamSegment.signals) {
				const auto converted = RenderGraphRuntime::NativeTimelinePoint(point);
				resolved = resolved && converted.semaphore;
				signals.push_back(converted);
			}
			auto user = std::make_unique<StreamSegment>(StreamSegment{ a_execution, index, streamDevice });
			if (!resolved || !RenderGraphRuntime::Get().EmitExternalCommands(&StreamSegmentCallback, user.get(), waits, signals, true, a_label, true)) {
				RenderGraphRuntime::Get().SignalExecutionUnexecuted(*a_execution, index, a_label);
				return false;
			}
			(void)user.release();  // the callback's
		}
		return true;
	}

	// The epoch's end. The stream waits for the compute queue's last signals. With a_split, the list holding
	// the segments the compute work waits for is flushed first.
	bool CloseComputeEpoch(const char* a_label, bool a_inStream = false, bool a_split = false)
	{
		streamPointEnqueued = false;
		computeEntered = false;
		copyEntered = false;
		if (computeTail.empty())
			return true;
		for (auto& wait : computeTail)
			wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		bool result = true;
		if (a_inStream) {
			if (a_split) {
				globals::d3d::context->Flush();
				NoteStreamClosed("exit split", a_label);
			}
			std::vector<RenderGraphRuntime::ExternalCommandsWait> waits;
			waits.reserve(computeTail.size());
			for (const auto& wait : computeTail)
				waits.push_back({ wait.semaphore, wait.value, wait.stageMask });
			result = RenderGraphRuntime::Get().EmitExternalCommands(&NoCommands, nullptr, waits, {}, false, a_label);
		} else if (batching) {
			if (pendingCount == pendingSubmits.size())
				pendingSubmits.emplace_back();
			auto& pending = pendingSubmits[pendingCount++];
			pending.waits = computeTail;
			pending.commandBuffers.clear();
			pending.signals.clear();
		} else {
			DxvkOrgInteropSubmission submission{};
			submission.version = DXVK_ORG_INTEROP_VERSION;
			submission.waitCount = static_cast<uint32_t>(computeTail.size());
			submission.waits = computeTail.data();
			submission.onSubmitted = &Impl::OnStreamSubmitted;
			submission.user = this;
			submission.label = a_label;
			streamEnqueued.fetch_add(1, std::memory_order_acq_rel);
			if (FAILED(enqueueSubmission(globals::d3d::device, &submission))) {
				streamEnqueued.fetch_sub(1, std::memory_order_acq_rel);
				result = false;
			} else {
				NoteEnqueued("exit wait", a_label, computeTail, {});
			}
		}
		computeTail.clear();
		return result;
	}

	// Render thread. Closes an external epoch nobody closed, after a flush, since the list may hold segments its compute work waits for.
	void CloseLeftOpenExternal()
	{
		if (!externalOpen)
			return;
		externalOpen = false;
		static std::atomic_flag logged;
		if (!logged.test_and_set())
			logger::error("[ORG] {}: an external epoch was not closed; its compute exit goes in at the next epoch, after a flush", externalLabel);
		RecordStreamEvent("external left open", externalLabel);
		if (!CloseComputeEpoch(externalLabel, true, true))
			logger::error("[ORG] {}: DXVK rejected the compute queue's exit wait", externalLabel);
	}

	// Every queue of DXVK's device, compute and copy included, is used under DXVK's submission lock. See computeQueue.
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
		if (a_result != VK_SUCCESS && !self->faulted.exchange(true)) {
			logger::error("[ORG] DXVK failed to submit render graph work ({}); disabling the graph", static_cast<int>(a_result));
			FaultIsFatal(fmt::format("DXVK failed to submit render graph work (VkResult {})", static_cast<int>(a_result)));
		}
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
			// Only other threads reach this, and the graph does not submit from them per frame, so polling is cheap.
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
		return true;
	}

	static const char* SegmentLabel(Segment a_segment)
	{
		// A fragment source's segment is named by its source.
		if (const char* label = RenderGraph::Frame::Revision::FragmentSegmentLabel(static_cast<std::uint32_t>(a_segment), false))
			return label;
		if (const auto* anchor = FrameAnchors::NamesOf(static_cast<std::uint32_t>(a_segment)))
			return anchor->label;
		return "CS render graph";
	}

	// Named after the segment's feature so the GPU idle trace sums it under that feature.
	static const char* InputsLabel(Segment a_segment)
	{
		if (const char* label = RenderGraph::Frame::Revision::FragmentSegmentLabel(static_cast<std::uint32_t>(a_segment), true))
			return label;
		if (const auto* anchor = FrameAnchors::NamesOf(static_cast<std::uint32_t>(a_segment)))
			return anchor->inputs;
		return "CS render graph: feature inputs";
	}

	static VkResult Submit(void* a_user, VkQueue a_queue, const VkSubmitInfo2& a_submit)
	{
		auto* self = static_cast<Impl*>(a_user);
		// The compute and copy queues are the graph's alone. Timeline waits on values still in DXVK's stream are legal.
		// On a copy queue shared with the uploader, only the render thread's submissions are the graph's.
		const bool copy = a_queue == self->copyQueue && (a_queue != self->uploadQueue || ::GetCurrentThreadId() == self->streamThread.load(std::memory_order_relaxed));
		if (copy || a_queue == self->computeQueue) {
			if (::GetCurrentThreadId() != self->streamThread.load(std::memory_order_relaxed)) {
				// Its entry point is a position in DXVK's stream, which only the D3D11 thread can take.
				logger::error("[ORG] A {}-queue submission came from a thread other than the render thread", copy ? "copy" : "compute");
				return VK_ERROR_UNKNOWN;
			}
			VkSubmitInfo2 submit = a_submit;
			bool& entered = copy ? self->copyEntered : self->computeEntered;
			if (!entered) {
				if (!self->streamPointEnqueued) {
					if (!self->EnqueueStreamPoint())
						return VK_ERROR_UNKNOWN;
					self->streamPointEnqueued = true;
				}
				entered = true;
				self->computeWaits.assign(a_submit.pWaitSemaphoreInfos, a_submit.pWaitSemaphoreInfos + a_submit.waitSemaphoreInfoCount);
				self->computeWaits.push_back({ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, self->streamPoint, self->streamPointValue,
					VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 });
				submit.waitSemaphoreInfoCount = static_cast<uint32_t>(self->computeWaits.size());
				submit.pWaitSemaphoreInfos = self->computeWaits.data();
			}
			// Also signal the queue's exit timeline, which the stream's exit wait uses.
			auto& submitter = copy ? self->copySubmitter : self->computeSubmitter;
			if (const VkResult failed = submitter.failure.load(std::memory_order_acquire); failed != VK_SUCCESS)
				return failed;
			const VkSemaphoreSubmitInfo exit{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, copy ? self->copyExit : self->computeExit,
				copy ? ++self->copyExitValue : ++self->computeExitValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
			ComputeSubmitter::Batch batch;
			batch.queue = a_queue;
			batch.waits.assign(submit.pWaitSemaphoreInfos, submit.pWaitSemaphoreInfos + submit.waitSemaphoreInfoCount);
			batch.commandBuffers.assign(submit.pCommandBufferInfos, submit.pCommandBufferInfos + submit.commandBufferInfoCount);
			batch.signals.assign(submit.pSignalSemaphoreInfos, submit.pSignalSemaphoreInfos + submit.signalSemaphoreInfoCount);
			batch.signals.push_back(exit);
			self->NoteOffStream(copy, batch.waits, batch.signals);
			submitter.Push(std::move(batch));
			const auto tail = std::ranges::find(self->computeTail, exit.semaphore, &VkSemaphoreSubmitInfo::semaphore);
			if (tail != self->computeTail.end())
				*tail = exit;
			else
				self->computeTail.push_back(exit);
			return VK_SUCCESS;
		}
		if (a_queue == self->uploadQueue) {
			if (a_queue == self->copyQueue)
				self->WaitForPoints(a_submit.pWaitSemaphoreInfos, a_submit.waitSemaphoreInfoCount, true);
			{
				// DXVK's submission lock covers every queue of its device: a long hold by DXVK shows here, on the upload thread only.
				ZoneScopedN("CS.ORG.UploadQueue.LockSubmissionQueue");
				self->interop->LockSubmissionQueue();
			}
			const VkResult result = self->queueSubmit2(a_queue, 1, &a_submit, VK_NULL_HANDLE);
			self->interop->ReleaseSubmissionQueue();
			return result;
		}
		if (::GetCurrentThreadId() == self->streamThread.load(std::memory_order_relaxed)) {
			if (self->batching) {
				if (self->pendingCount == self->pendingSubmits.size())
					self->pendingSubmits.emplace_back();
				auto& pending = self->pendingSubmits[self->pendingCount++];
				pending.waits.assign(a_submit.pWaitSemaphoreInfos, a_submit.pWaitSemaphoreInfos + a_submit.waitSemaphoreInfoCount);
				pending.commandBuffers.assign(a_submit.pCommandBufferInfos, a_submit.pCommandBufferInfos + a_submit.commandBufferInfoCount);
				pending.signals.assign(a_submit.pSignalSemaphoreInfos, a_submit.pSignalSemaphoreInfos + a_submit.signalSemaphoreInfoCount);
				return VK_SUCCESS;
			}
			// In D3D11 stream order, after every D3D11 command so far. Nothing is waited for here.
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
			// Nsight and RenderDoc group the epoch's command buffers under this label. DXVK adds it only under a capture tool.
			submission.label = SegmentLabel(RenderGraphRuntime::Get().CurrentSegment());
			self->streamEnqueued.fetch_add(1, std::memory_order_acq_rel);
			if (FAILED(self->enqueueSubmission(globals::d3d::device, &submission))) {
				self->streamEnqueued.fetch_sub(1, std::memory_order_acq_rel);
				return VK_ERROR_UNKNOWN;
			}
			self->NoteEnqueued("graph submission", submission.label, { a_submit.pWaitSemaphoreInfos, a_submit.waitSemaphoreInfoCount },
				{ a_submit.pSignalSemaphoreInfos, a_submit.signalSemaphoreInfoCount });
			return VK_SUCCESS;
		}

		// Another thread. Let DXVK's stream reach the queue first to keep order, then submit under DXVK's queue lock.
		// With async epochs only the render thread should submit, so this is counted.
		if (self->offThreadSubmits.fetch_add(1, std::memory_order_relaxed) == 0)
			logger::warn("[ORG] A graph submission came from a thread other than the render thread while epochs are async");
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
	// The shared table resolves the same module.
	static_cast<void>(a_dxvkD3D11);
	const auto& dxvk = DxvkExports::Get();
	// Graph mode has both: DxvkLoader stops it without them. CS_ORG=features and VR may run on a DXVK without them.
	if (!dxvk.requestDeviceFeatures || !dxvk.setBindingModelCallback)
		return;
	const DxvkOrgInteropFeatureRequest desc{ DXVK_ORG_INTEROP_VERSION, static_cast<uint32_t>(std::size(kRequestedFeatures)), kRequestedFeatures };
	if (FAILED(dxvk.requestDeviceFeatures(&desc)))
		logger::warn("[ORG] DXVK rejected the render graph device feature request");
	const DxvkOrgInteropBindingModelNegotiation negotiation{ DXVK_ORG_BINDING_MODEL_NEGOTIATION_VERSION, &NegotiateBindingModel, nullptr };
	if (FAILED(dxvk.setBindingModelCallback(&negotiation)))
		logger::warn("[ORG] DXVK rejected the render graph's binding model callback");
}

std::shared_ptr<org::Runtime> RenderGraphRuntime::CreateOrgRuntime(const rhi::DevicePtr& a_device)
{
	// Diagnostic: CS_ORG_ALIAS_LOG=1 logs every alias plan's placements, ranks and exclusions.
	if (Switches::Equals("CS_ORG_ALIAS_LOG", "1")) {
		auto settings = org::runtime::GetOpenRenderGraphSettings();
		settings.autoAliasEnableLogging = true;
		org::runtime::SetOpenRenderGraphSettings(settings);
	}
#if defined(CS_RELEASE_PROFILE)
	// No pass timestamps and no readback of them: the statistics take this when the runtime below creates them.
	auto settings = org::runtime::GetOpenRenderGraphSettings();
	settings.collectPassStatistics = false;
	settings.collectPipelineStatistics = false;
	org::runtime::SetOpenRenderGraphSettings(settings);
#endif
	// The runtime's first run capacity. Its per-lease lists grow past it. ORG's tasks run on CS's executor.
	// A failure deep in ORG's services (an unmapped staging page, the uploader's thread stopping) is fatal.
	return org::Runtime::Create({ .device = a_device.Get(),
		.backend = RenderGraph::Backend::Active(),
		.slots = kRunCapacity,
		.tasks = RenderGraph::Frame::OrgTaskService(),
		.onError = RenderGraph::FatalHandler("ORG runtime") });
}

bool RenderGraphRuntime::Initialize()
{
	if (impl)
		return true;
	if (attempted)
		return false;
	attempted = true;
	// OMS's programs read CS's shader folder, backend, compiler and defines through it, before any program loads.
	RenderGraph::Programs::InstallEnvironment();

	auto disable = [&](std::string a_reason) {
		disabledReason = std::move(a_reason);
		logger::info("[ORG] Render graph disabled: {}", disabledReason);
		impl.reset();
		FaultIsFatal(fmt::format("it could not start ({})", disabledReason));
		return false;
	};

	if (EnvDisabled())
		return disable("CS_ORG=0");
	if (EnvFeaturesOnly())
		return disable("CS_ORG=features (device features requested, graph not created)");
	// The frame's epochs come from Drawcall Limit Fix's schedule. The runtime has no orders of its own.
	if (!frameConfigured)
		return disable("Drawcall Limit Fix did not configure the frame's epochs (it did not install: CS_DCLF=0 or an unsupported game)");
	// CS_ORG_NATIVE_BACKEND=d3d12 without native mode or a D3D12 build is an error, never a silent Vulkan fallback.
	if (auto backendError = RenderGraph::Backend::RequestError(); !backendError.empty()) {
		logger::critical("[ORG] {}", backendError);
		return disable(std::move(backendError));
	}
	// CS_ORG_NATIVE=1 uses native mode's device and no DXVK. Epochs submit straight to its queues, with no stream or queue boundaries.
	const bool native = RenderGraph::Engine::Native::Active();
	if (!native && !DxvkLoader::IsLoaded())
		return disable("DXVK is not loaded (native D3D11)");

	HMODULE d3d11 = native ? nullptr : ::GetModuleHandleW(L"dxvk_d3d11.dll");
	// Native mode resolves nothing from DXVK.
	static const DxvkExports noDxvk{};
	const auto& dxvk = native ? noDxvk : DxvkExports::Get();
	auto state = std::make_unique<Impl>();
	DxvkOrgInteropDeviceInfo info{};
	info.version = DXVK_ORG_INTEROP_VERSION;
	if (native) {
		state->native = true;
		state->device = RenderGraph::Engine::Native::Device();
		if (!state->device || !*state->device)
			return disable("native mode (CS_ORG_NATIVE=1) has no BasicRHI device");
		if (RenderGraph::Backend::IsD3D12()) {
			// The vendor, for the shaders' workarounds (CS_GPU_AMD) and the reports, from BasicRHI's adapter description.
			AdapterFeatureInfo adapter{};
			if ((*state->device)->QueryFeatureInfo(&adapter.header) == rhi::Result::Ok) {
				adoptedVendorId.store(adapter.vendorId, std::memory_order_relaxed);
				logger::info("[RenderGraph] D3D12 device (native mode): {} (vendor {:#06x}, device {:#06x})", adapter.name, adapter.vendorId, adapter.deviceId);
			}
		} else if (const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
					   LoaderProcAddr()(rhi::vulkan::get_instance(state->device->Get()), "vkGetPhysicalDeviceProperties"))) {
			VkPhysicalDeviceProperties properties{};
			getProperties(rhi::vulkan::get_physical_device(state->device->Get()), &properties);
			adoptedVendorId.store(properties.vendorID, std::memory_order_relaxed);
			logger::info("[RenderGraph] Vulkan device (native mode): {} (vendor {:#06x}, device {:#06x}), Vulkan {}.{}.{}", properties.deviceName, properties.vendorID,
				properties.deviceID, VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion));
		}
		RenderGraph::Engine::Native::Stage("render graph", "the graph host runs natively on BasicRHI's device (epochs submit directly)");
	} else {
		// DxvkLoader stops graph mode on a DXVK without every export. This covers VR, where the graph may start outside graph mode.
		if (const auto missing = dxvk.Missing(); !missing.empty())
			return disable(fmt::format("the DXVK build lacks exports the render graph needs ({})", missing));
		auto getInfo = dxvk.getInteropDeviceInfo;
		state->createBufferFromVkBuffer = dxvk.createBufferFromVkBuffer;
		state->setTeardownCallback = dxvk.setDeviceTeardownCallback;
		// Graph textures that D3D11 still reads (GraphResources' exports), and engine textures made on graph images (Engine/Textures).
		state->createTextureFromVkImage = dxvk.createTextureFromVkImage;
		state->createTextureFromVkImageWithData = dxvk.createTextureFromVkImageWithData;
		// Game resources for the graph (Drawcall Limit Fix).
		state->getResourceInfo = dxvk.getInteropResourceInfo;
		state->getResourceInfos = dxvk.getInteropResourceInfos;
		state->trackResources = dxvk.trackInteropResources;
		state->untrackResources = dxvk.untrackInteropResources;
		state->pollRelocations = dxvk.pollInteropRelocations;
		state->releaseStorage = dxvk.releaseInteropStorage;
		// Recordings into DXVK's own command buffer (EmitCommands, the in-stream epochs).
		state->emitExternalCommands = dxvk.emitExternalCommands;
		// Graph submissions in DXVK's command stream.
		state->enqueueSubmission = dxvk.enqueueInteropSubmission;
		state->enqueueSubmissions = dxvk.enqueueInteropSubmissions;

		auto d3dDevice = globals::d3d::device;
		if (!d3dDevice || FAILED(d3dDevice->QueryInterface(__uuidof(IDXGIVkInteropDevice), state->interop.put_void())))
			return disable("IDXGIVkInteropDevice is unavailable");

		// Resting layouts keep imported images in DXVK's own layouts instead of GENERAL, which keeps compression on AMD's Windows driver.
		// They need external home states too.
		{
			if (FAILED(dxvk.setInteropOptions(d3dDevice, DXVK_ORG_INTEROP_RESTING_LAYOUTS)))
				return disable("DXVK rejected resting layouts for imported images (dxvkSetInteropOptions)");
			logger::info("[ORG] imported images rest in DXVK's layouts");
		}

		if (FAILED(getInfo(d3dDevice, &info)))
			return disable("DXVK could not describe its Vulkan device");
		// The device, once, ahead of any reason the graph is disabled: a report from another machine is read against it.
		if (const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(info.getInstanceProcAddr(info.instance, "vkGetPhysicalDeviceProperties"))) {
			VkPhysicalDeviceProperties properties{};
			getProperties(info.physicalDevice, &properties);
			adoptedVendorId.store(properties.vendorID, std::memory_order_relaxed);
			const auto driver = properties.driverVersion;
			// NVIDIA packs its driver version 10.8.8.6; the others use Vulkan's packing.
			const auto driverText = properties.vendorID == 0x10DE ? fmt::format("{}.{:02}", driver >> 22, (driver >> 14) & 0xFF) :
			                                                        std::format("{}.{}.{}", VK_API_VERSION_MAJOR(driver), VK_API_VERSION_MINOR(driver), VK_API_VERSION_PATCH(driver));
			logger::info("[RenderGraph] Vulkan device: {} (vendor {:#06x}, device {:#06x}), driver {}, Vulkan {}.{}.{}, {} extensions enabled", properties.deviceName,
				properties.vendorID, properties.deviceID, driverText, VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
				VK_API_VERSION_PATCH(properties.apiVersion), info.enabledExtensionCount);
		}
		// BasicRHI runs on DXVK's negotiated model, descriptor buffers with mutable descriptors. DXVK grants nothing on legacy sets.
		const auto* bufferFeatures = FindFeatures<VkPhysicalDeviceDescriptorBufferFeaturesEXT>(info.enabledFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT);
		const auto* mutableFeatures = FindFeatures<VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT>(info.enabledFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT);
		const bool onBuffers = HasExtension(info, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME) && HasExtension(info, VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME) &&
		                       bufferFeatures && bufferFeatures->descriptorBuffer && mutableFeatures && mutableFeatures->mutableDescriptorType;
		if (!onBuffers) {
			// The one cause seen in practice, named so the menu does not leave it to be guessed.
			if (::GetModuleHandleW(L"renderdoc.dll"))
				return disable("DXVK runs on legacy descriptor sets: RenderDoc is loaded, and its DXVK capture config turns off descriptor buffers, which the "
							   "render graph needs. Turn off RenderDoc capture to use the render graph.");
			return disable("DXVK did not grant descriptor buffers with mutable descriptors: it runs on legacy descriptor sets, or on a model BasicRHI cannot "
						   "use on this device (see the binding model negotiation above and DXVK's 'Binding model' and 'Interop feature denied' log lines)");
		}
		logger::info("[ORG] Binding model: descriptor buffer (VK_EXT_descriptor_buffer + VK_EXT_mutable_descriptor_type)");

		rhi::vulkan::AdoptedVulkanDeviceInfo adopt{};
		adopt.getInstanceProcAddr = info.getInstanceProcAddr;
		adopt.instance = info.instance;
		adopt.instanceApiVersion = info.instanceApiVersion;
		adopt.physicalDevice = info.physicalDevice;
		adopt.device = info.device;
		adopt.enabledDeviceExtensions = info.enabledExtensions;
		adopt.enabledDeviceExtensionCount = info.enabledExtensionCount;
		{
			bool budget = false;
			for (std::uint32_t i = 0; i < info.enabledExtensionCount; ++i)
				budget = budget || std::string_view(info.enabledExtensions[i]) == VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
			logger::info("[ORG] adopted device: {} extensions, VK_EXT_memory_budget {}", info.enabledExtensionCount, budget ? "on" : "off");
		}
		adopt.diagnostics.registerMarker = &Aftermath::RegisterOrgCheckpoint;
		// DXVK's hang mode turns on BasicRHI's own crash markers and the detailed checkpoints.
		adopt.diagnostics.crashMarkers = Switches::Equals("DXVK_DEBUG", "hang");
		adopt.diagnostics.detailedCommands = adopt.diagnostics.crashMarkers;
		// Whether VK_EXT_debug_utils is on, so that BasicRHI names objects and labels passes only then.
		adopt.enabledInstanceExtensions = info.enabledInstanceExtensions;
		adopt.enabledInstanceExtensionCount = info.enabledInstanceExtensionCount;
		adopt.enabledFeatureChain = info.enabledFeatures;
		// Exactly what the negotiation evaluated (GraphBindingRequirements).
		adopt.bindingRequirements = GraphBindingRequirements(info.getInstanceProcAddr, info.instance, info.physicalDevice);
		adopt.queues[0] = { info.graphicsQueue, info.graphicsQueueFamily, info.graphicsQueueIndex };
		adopt.submissionHooks = { state.get(), &Impl::LockQueue, &Impl::UnlockQueue, nullptr };
		{
			// Graph submissions go through DXVK's stream (Impl::Submit), and the off-stream queues submit with vkQueueSubmit2.
			auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(info.getInstanceProcAddr(info.instance, "vkGetDeviceProcAddr"));
			state->queueSubmit2 = getDeviceProcAddr ? reinterpret_cast<PFN_vkQueueSubmit2>(getDeviceProcAddr(info.device, "vkQueueSubmit2")) : nullptr;
			if (!state->queueSubmit2)
				return disable("DXVK's Vulkan device has no vkQueueSubmit2");
			state->deviceProcAddr = getDeviceProcAddr;
			state->adoptedDevice = info.device;
			adopt.submissionHooks.submit = &Impl::Submit;
			state->streamThread = ::GetCurrentThreadId();
			// The frame-pace timeline (SignalFramePace): signalled from DXVK's stream, so it needs the stream.
			{
				if (Switches::IsSet("CS_FRAME_LATENCY"))
					state->frameLatency = static_cast<uint32_t>(std::clamp<std::uint64_t>(Switches::Number("CS_FRAME_LATENCY", 0), 0, kGameFramesInFlight));
				const auto createSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(getDeviceProcAddr(info.device, "vkCreateSemaphore"));
				state->destroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(getDeviceProcAddr(info.device, "vkDestroySemaphore"));
				state->waitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(getDeviceProcAddr(info.device, "vkWaitSemaphores"));
				VkSemaphoreTypeCreateInfo type{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
				type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
				VkSemaphoreCreateInfo create{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
				create.pNext = &type;
				if (state->frameLatency && createSemaphore && state->destroySemaphore && state->waitSemaphores &&
					createSemaphore(info.device, &create, nullptr, &state->framePace) == VK_SUCCESS)
					state->vkDevice = info.device;
				logger::info("[ORG] frame pacing: {}", state->framePace ? std::format("the GPU at most {} frames behind the render thread (CS_FRAME_LATENCY)", state->frameLatency - 1) : std::string("off"));
			}
			// DXVK's compute-only queue, ordered against the stream by the host (EnqueueStreamPoint, CloseComputeEpoch), which
			// needs the stream.
			if (info.computeQueue && getDeviceProcAddr) {
				const auto createSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(getDeviceProcAddr(info.device, "vkCreateSemaphore"));
				state->destroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(getDeviceProcAddr(info.device, "vkDestroySemaphore"));
				state->waitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(getDeviceProcAddr(info.device, "vkWaitSemaphores"));
				VkSemaphoreTypeCreateInfo type{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
				type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
				VkSemaphoreCreateInfo create{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
				create.pNext = &type;
				if (createSemaphore && state->destroySemaphore && state->waitSemaphores &&
					createSemaphore(info.device, &create, nullptr, &state->streamPoint) == VK_SUCCESS &&
					createSemaphore(info.device, &create, nullptr, &state->computeExit) == VK_SUCCESS) {
					state->vkDevice = info.device;
					state->computeQueue = info.computeQueue;
					state->computeSubmitter.Start(*state, L"CS ORG compute submit", false);
					adopt.queues[1] = { info.computeQueue, info.computeQueueFamily, info.computeQueueIndex };
					// Graph copies move off DXVK's stream to a transfer-only queue, DXVK's spare or the shared uploader's.
					// Never the compute queue, where a copy waits behind a running dispatch.
					const bool uploadOffered = adopt.submissionHooks.submit && info.uploadQueue;
					const bool shared = !info.copyQueue && uploadOffered;
					if ((info.copyQueue || shared) && createSemaphore(info.device, &create, nullptr, &state->copyExit) == VK_SUCCESS) {
						state->copyQueue = shared ? info.uploadQueue : info.copyQueue;
						adopt.queues[2] = shared ? rhi::vulkan::AdoptedQueue{ info.uploadQueue, info.uploadQueueFamily, info.uploadQueueIndex } :
						                           rhi::vulkan::AdoptedQueue{ info.copyQueue, info.copyQueueFamily, info.copyQueueIndex };
						state->copySubmitter.Start(*state, L"CS ORG copy submit", shared);
					}
				}
			}
		}
		// The upload queue, offered to ORG's copy-queue uploader. Its submissions are direct, under the lock.
		rhi::vulkan::AdoptedQueue uploadSpare{};
		if (adopt.submissionHooks.submit && info.uploadQueue) {
			state->uploadQueue = info.uploadQueue;
			uploadSpare = { info.uploadQueue, info.uploadQueueFamily, info.uploadQueueIndex };
			adopt.spareQueues = &uploadSpare;
			adopt.spareQueueCount = 1;
		}
		logger::info("[ORG] Queues: graphics {}:{}, compute {}, upload {}, copy {}", info.graphicsQueueFamily, info.graphicsQueueIndex,
			state->computeQueue ? fmt::format("{}:{}", info.computeQueueFamily, info.computeQueueIndex) : std::string("shared with graphics"),
			state->uploadQueue ? fmt::format("{}:{}", info.uploadQueueFamily, info.uploadQueueIndex) : std::string("none"),
			!state->copyQueue ? std::string("shared with graphics") : state->copyQueue == state->uploadQueue ? std::string("shared with upload") : fmt::format("{}:{}", info.copyQueueFamily, info.copyQueueIndex));
		if (rhi::vulkan::AdoptVulkanDevice(adopt, *state->device) != rhi::Result::Ok || !*state->device)
			return disable("BasicRHI could not adopt DXVK's Vulkan device");
	}
	// CS_SUBMIT_TRACE=<file> turns on BasicRHI's submission trace before any list is recorded, so every kept list is named.
	if (char path[8]{}; GetEnvironmentVariableA("CS_SUBMIT_TRACE", path, sizeof(path)))
		rhi::submittrace::Enable(true);
	if ((*state->device)->CreateTimeline(state->nativeTimelines, 0, "CS native timeline lookup") != rhi::Result::Ok || !state->nativeTimelines)
		return disable("BasicRHI could not create a timeline on DXVK's Vulkan device");

	try {
		org::GraphHost::Desc desc{};
		// Barriers order the graph against DXVK's work before and after each epoch on the shared queue.
		desc.queueBoundary = { .entry = !state->native, .exit = !state->native };
		// Game frames in flight, for frame pacing and published copies. Every run takes its own run lease.
		desc.framesInFlight = kGameFramesInFlight;
		// Guest mode returns DXVK's imported images to their home layouts on the graphics queue after each epoch.
		// Native mode has no home states. The ledgers carry each image's layout between executions.
		desc.externalHomeStates = !state->native;
		if (state->native)
			logger::info("[ORG] native: no home states; the graph imports native images exclusively and its ledgers carry their layouts");
		// Tickets record replayable recordings, which the revision-driven epochs need (their published copies are submitted again).
		// A live run also reuses its copy's kept recording while every pass revision holds.
		desc.reuseRecordings = true;
		// The commit runs on the render thread and the rest of the submission on a host thread.
		desc.submissionThread = state->native;
		if (desc.submissionThread)
			logger::info("[ORG] native: epochs submit on a submission thread");
		// Tickets queued together share queue submissions where they can.
		desc.mergeTicketSubmissions = state->native;
		// The seal and present queue on the submission thread after the frame's epochs.
		// The render thread waits only when the previous present is still queued.
		desc.queuedSeals = state->native;
		// Uploads are recorded on the compute queue when that queue uses them first. A pass must declare every upload it reads.
		// DCLF's per-frame publications are read through late-bound buffers for this. An undeclared read once hung the RX 6600.
		desc.queueScopedUploads = true;
		// Stale alias placements are rebuilt off the render thread by the build manager, like other rebuilds.
		desc.reportStaleAliasPlacements = true;
		// The segments in frame order, from DCLF's schedule.
		desc.epochOrder = EpochsOf(frameConfig.epochOrder);
		// The CS stages' epochs, where the frame reaches them (FrameAnchors.h).
		FrameAnchors::AddToFrame(desc.epochOrder);
		if (!desc.epochOrder.empty())
			logger::info("[ORG] epoch order: {}", fmt::join(desc.epochOrder, ","));
		// Native mode made the runtime with its device, for its external client.
		state->orgRuntime = state->native ? RenderGraph::Engine::Native::TakeOrgRuntime() : CreateOrgRuntime(*state->device);
		if (!state->orgRuntime)
			return disable("native mode made no ORG runtime with its device");
		desc.runtime = state->orgRuntime;
		state->host = std::make_unique<org::GraphHost>(std::move(desc));
		// The build manager makes every rebuild off the render thread and attaches it at a frame boundary.
		// An epoch with no graph is still built at its first submission.
		state->host->SetExplicitBuilds(true);
		// The frame-wait timeline, made before any build because packets bind it. It needs an uploader queue to signal from.
		// DCLF's frame values need it.
		if (state->uploadQueue || state->native) {
			auto timeline = std::make_shared<rhi::TimelinePtr>();
			if ((*state->device)->CreateTimeline(*timeline, 0, "CS frame values") == rhi::Result::Ok && *timeline) {
				state->frameWaitTimeline = timeline;
				state->host->SetFrameWaitTimeline(std::move(timeline));
			} else {
				logger::warn("[ORG] The frame-wait timeline could not be created: per-frame values have no GPU wait");
			}
		}
	} catch (const std::exception& e) {
		return disable(std::string("graph host creation failed: ") + e.what());
	}

	if (state->setTeardownCallback)
		state->setTeardownCallback(&Impl::OnDxvkTeardown, nullptr);
	// CS_ORG_RECORDING_TIMESTAMPS also times the recordings kept across frames. Native stages are all kept recordings,
	// so without it they report nothing while GPU timings are collected.
	if (Switches::Equals("CS_ORG_RECORDING_TIMESTAMPS", "1") || Switches::EqualsNoCase("CS_ORG_RECORDING_TIMESTAMPS", "passes"))
		state->host->SetRecordingTimestamps(true);
	else if (Switches::EqualsNoCase("CS_ORG_RECORDING_TIMESTAMPS", "lists") ||
			 (!Switches::Equals("CS_ORG_RECORDING_TIMESTAMPS", "0") && GpuTimingSource::Collecting()))
		state->host->SetRecordingTimestampMode(org::RenderGraph::RecordingTimestampMode::Lists);
	state->host->SetCompletedFrameCallback([self = state.get()](std::uint64_t a_frameNumber, const org::runtime::IStatisticsService& a_stats) {
		self->OnCompletedFrame(a_frameNumber, a_stats);
	});
	// Every submission up to a_submissions is done on the GPU, so pooled versions can go.
	// Runs on the host's thread with async epochs.
	state->host->SetRetiredCallback([self = state.get()](std::uint64_t a_submissions) {
		self->retiredSubmissions.store(a_submissions, std::memory_order_release);
		// With CPU pipeline frames, a point retires only once its frames' signals did too.
		RenderGraph::Frame::Retirement::NoteGraphSubmissionsRetired(a_submissions);
	});
	// One epoch failed to build or prepare. The others run on, and its owner decides what to do.
	state->host->SetEpochFailedCallback([this](const org::GraphHost::EpochFailure& a_failure) {
		OnEpochFailed(a_failure.epoch, a_failure.keptGraph, a_failure.error);
	});
	// Every graph build pins the pipeline versions its recordings bind, so a shader reload never changes what a graph or a kept
	// recording binds (cleanup plan A4). The revision that carries a reload's ShaderGeneration adopts the rebuilt graphs.
	RenderGraph::PipelineVersions::Attach(*state->host);
	RenderGraph::PipelineVersions::Install();
	// The frame anchors' passes, before any graph is built (so adding the extension rebuilds nothing).
	FrameAnchors::Attach(*state->host);
	{
		try {
			// The async epochs run in DCLF's frame order. Its revision-driven epochs need no live ticket: each submission carries
			// its revision's recording.
			std::vector<std::uint32_t> epochs = EpochsOf(frameConfig.epochOrder);
			std::vector<std::uint32_t> revisionEpochs = EpochsOf(frameConfig.revisionEpochs);
			// The frame anchors' epochs are live async epochs, in DXVK's stream with the others.
			FrameAnchors::AddToFrame(epochs);
			// The CS stages are revision-driven and recorded per description. Their entry barriers come from the ledgers at each submission.
			if (const auto segments = FrameAnchors::SegmentEpochs(); !segments.empty()) {
				revisionEpochs.insert(revisionEpochs.end(), segments.begin(), segments.end());
				// Stage transients go in the frame arena, each stage in its own reserved range, so alias plans repeat every frame.
				// In a shared arena another epoch took their memory between frames, and every admission replanned at about twice the cost.
				if (!RenderGraph::Frame::Transients::Install(*state->host))
					state->host->SetPrivateAliasEpochs(segments);
				logger::info("[ORG] frame segments: epochs {} are revision-driven and open", fmt::join(segments, ","));
			}
			logger::info("[ORG] async epochs: {}; revision-driven: {}", fmt::join(epochs, ","), fmt::join(revisionEpochs, ","));
			// Guest mode runs the async epochs in DXVK's stream. Native mode has no external epochs,
			// and passes recorded at execution go inline into the epoch's own list.
			if (state->native) {
				state->host->SetInlineAtExecution(true);
				logger::info(
					"[ORG] native: no external epochs; every epoch submits directly, and passes recorded at execution are recorded "
					"inline into their epoch's command list");
			}
			if (!state->native) {
				state->inStreamEpochs = epochs;
				std::ranges::sort(state->inStreamEpochs);
				logger::info("[ORG] in-stream epochs: {} (their graphics work in DXVK's command list, split only where a wait needs it)",
					fmt::join(state->inStreamEpochs, ","));
			}
			state->host->SetExternalEpochs(state->inStreamEpochs);
			state->host->SetAsyncEpochs(std::move(epochs), std::move(revisionEpochs));
			logger::info("[ORG] Async epochs: each epoch is prepared and recorded ahead on the graph host's thread; the render thread submits (the fragment sources' from their fragments)");
		} catch (const std::exception& e) {
			return disable(std::string("async epochs unavailable: ") + e.what());
		}
	}
	{
		// Fragment sources' epochs build on the build manager's lane like the others, their first builds too. They are staged, and
		// attach with the fragment recorded on them at a Begin (Frame::Revision::RegisterFragmentSource).
		RenderGraph::BuildManager::Desc builds{};
		builds.host = state->host.get();
		for (const auto fragmentSegment : RenderGraph::Frame::Revision::FragmentSegments())
			if (fragmentSegment < static_cast<std::uint32_t>(Segment::Count)) {
				const auto epoch = EpochOf(static_cast<Segment>(fragmentSegment));
				if (std::ranges::find(builds.laneOnly, epoch) == builds.laneOnly.end())
					builds.laneOnly.push_back(epoch);
			}
		// A finished stage build is staged and recorded on the host's thread, then switched in whole at a frame boundary, so it never blanks a stage.
		builds.staged = FrameAnchors::SegmentEpochs();
		for (const auto epoch : builds.laneOnly)
			if (std::ranges::find(builds.staged, epoch) == builds.staged.end())
				builds.staged.push_back(epoch);
		builds.label = [](std::uint32_t a_epoch) -> std::string {
			return a_epoch < static_cast<std::uint32_t>(Segment::Count) ? std::string(Impl::SegmentLabel(static_cast<Segment>(a_epoch))) :
			                                                              fmt::format("epoch {}", a_epoch);
		};
		// The frame anchors handle their own failed builds. Other epochs' owners hear of it like any epoch failure.
		builds.onFailed = [this](std::uint32_t a_epoch, bool a_kept, const std::string& a_error) {
			if (!FrameAnchors::NamesOf(a_epoch))
				OnEpochFailed(a_epoch, a_kept, a_error);
		};
		state->builds = RenderGraph::MakeBuildManager(std::move(builds));
	}
	impl = std::move(state);
	disabledReason.clear();
	if (impl->native) {
		logger::info("[ORG] Render graph runs natively on BasicRHI's {} device; submissions go to its queues directly",
			RenderGraph::Backend::Name(RenderGraph::Backend::Active()));
		// Nsight Perf's Vulkan backend on the native device: frame boundaries under the native queue lock, the graph's passes as ranges.
		if (DxvkOrgInteropDeviceInfo nativeDevice{}; RenderGraph::Engine::Native::DescribeVulkanDevice(nativeDevice))
			GpuCounters::RegisterVulkanDevice(nativeDevice, nullptr, *impl->host);
		else
			logger::info("[GpuCounters] native mode on D3D12: no hardware counters (the Vulkan backend records into Vulkan command buffers)");
	} else {
		// Nsight Perf's Vulkan backend: its captures run on DXVK's graphics queue, with the graph's passes as ranges.
		GpuCounters::RegisterVulkanDevice(info, d3d11, *impl->host);
		logger::info("[ORG] Render graph adopted DXVK's Vulkan device (queue family {}, index {}); submissions go through DXVK's command stream",
			info.graphicsQueueFamily, info.graphicsQueueIndex);
	}
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
	{
		// Load DXC from beside the DXVK DLLs, not by name. Another dxcompiler.dll without SPIR-V may already be loaded.
		wchar_t dxvkPath[MAX_PATH]{};
		std::filesystem::path compilerDirectory;
		// Native mode: the same directory, CS's runtime DLLs, without DXVK in it.
		if (impl->native)
			compilerDirectory = RenderGraph::Engine::Native::RuntimeDirectory();
		else if (::GetModuleFileNameW(d3d11, dxvkPath, MAX_PATH))
			compilerDirectory = std::filesystem::path(dxvkPath).parent_path();
		impl->shaderCompiler = std::make_unique<org::services::ShaderCompiler>(std::filesystem::path{}, compilerDirectory, Util::SharedShaderStore());
		// A reload means files may have changed, so the store must read them again. This listener also runs after a publish, which changes no files.
		PipelineRegistry()->AddListener([sourcesGeneration = PipelineRegistry()->SourcesGeneration()]() mutable {
			const auto current = PipelineRegistry()->SourcesGeneration();
			if (current == sourcesGeneration)
				return;
			sourcesGeneration = current;
			Util::BeginShaderStoreGeneration();
		});
		logger::info("[ORG] Runtime SPIR-V compilation {}", impl->shaderCompiler->Available() ? "available" : "unavailable (no dxcompiler.dll beside the DXVK DLLs)");
		if (!impl->shaderCompiler->Available())
			impl->shaderCompiler.reset();
	}
#endif
	if (impl->native)
		RenderGraph::Engine::Native::OnShaderCompilerReady();
	IndirectCommandsFeatureInfo indirect{};
	if ((*impl->device)->QueryFeatureInfo(&indirect.header) == rhi::Result::Ok) {
		logger::info("[ORG] Indirect commands: generated commands {}, index buffer arguments {}, pipeline sets {} (up to {} pipelines); game resource imports {}",
			indirect.constantArguments, indirect.indexBufferArguments, indirect.pipelineSets, indirect.maxPipelineSetCount,
			impl->native ? "native" : impl->trackResources ? "pinless (relocations followed)" : "pinned");
	}
	return true;
}

void RenderGraphRuntime::Shutdown()
{
	// No program build may create a pipeline on a device being torn down (ProgramService).
	RenderGraph::Programs::Stop();
	if (!impl)
		return;
	// Let the build thread finish and drop unattached graphs before anything of the host goes.
	if (impl->builds)
		impl->builds->Shutdown();
	// CS's external client waits for its submissions while the runtime it shares with the host is whole.
	RenderGraph::ExternalWork::Shutdown();
	auto state = std::move(impl);
	if (state->setTeardownCallback)
		state->setTeardownCallback(nullptr, nullptr);
	// Graph work in DXVK's stream must reach the queue before the host waits on its timelines.
	// If DXVK dropped it the wait would never end, so leak the graph as at process exit.
	if (state->enqueueSubmission && !state->WaitForStreamSubmissions(std::chrono::seconds(5))) {
		logger::warn("[ORG] Queued render graph submissions never reached the queue; leaking the graph");
		if (state->device && *state->device)
			rhi::vulkan::abandon_device(state->device->Get());
		(void)state->host.release();
		(void)state.release();
		disabledReason = "shut down";
		return;
	}
	// The compute and copy queues' last submissions, before the host waits on their timelines.
	state->computeSubmitter.Stop();
	state->copySubmitter.Stop();
	// DXVK's device is going, so wait for the GPU once and free exported resources while their graph objects still have a device.
	if (!state->d3d11Releases.empty()) {
		if (state->device && *state->device)
			(void)state->device->Get().WaitIdle();
		state->d3d11Releases.clear();
	}
	try {
		// Waits only for the graph's own timelines, then releases every graph object.
		state->host.reset();
	} catch (const std::exception& e) {
		logger::error("[ORG] Render graph shutdown failed: {}", e.what());
	}
	// The pipeline versions no graph pins any more, while the device is still there (its work is done).
	PipelineRegistry()->DrainRetired();
	if (state->streamPoint) {
		// Every point was waited for by a compute submission the graph's timelines have seen complete.
		VkSemaphoreWaitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
		wait.semaphoreCount = 1;
		wait.pSemaphores = &state->streamPoint;
		wait.pValues = &state->streamPointValue;
		(void)state->waitSemaphores(state->vkDevice, &wait, UINT64_MAX);
		state->destroySemaphore(state->vkDevice, state->streamPoint, nullptr);
	}
	if (state->computeExit) {
		// Its last value is the last compute submission's; the stream's waits on it reached the queue above.
		VkSemaphoreWaitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
		wait.semaphoreCount = 1;
		wait.pSemaphores = &state->computeExit;
		wait.pValues = &state->computeExitValue;
		(void)state->waitSemaphores(state->vkDevice, &wait, UINT64_MAX);
		state->destroySemaphore(state->vkDevice, state->computeExit, nullptr);
	}
	if (state->copyExit) {
		// The same for the copy queue.
		VkSemaphoreWaitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
		wait.semaphoreCount = 1;
		wait.pSemaphores = &state->copyExit;
		wait.pValues = &state->copyExitValue;
		(void)state->waitSemaphores(state->vkDevice, &wait, UINT64_MAX);
		state->destroySemaphore(state->vkDevice, state->copyExit, nullptr);
	}
	if (state->framePace) {
		// Its last signal reaches the queue with DXVK's next flush; bounded, since a teardown may never flush again.
		VkSemaphoreWaitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
		wait.semaphoreCount = 1;
		wait.pSemaphores = &state->framePace;
		wait.pValues = &state->framePaceValue;
		if (state->waitSemaphores(state->vkDevice, &wait, 1'000'000'000ull) == VK_SUCCESS)
			state->destroySemaphore(state->vkDevice, state->framePace, nullptr);
	}
	try {
		state->orgRuntime.reset();
	} catch (const std::exception& e) {
		logger::error("[ORG] Runtime shutdown failed: {}", e.what());
	}
	state->nativeTimelines.Reset();
	// Non-owning: BasicRHI never destroys the adopted VkDevice.
	state->device.reset();
	disabledReason = "shut down";
}

RenderGraphRuntime::~RenderGraphRuntime()
{
	// Static destruction at process exit: DXVK may already be gone, so no Vulkan
	// call is safe. Leak the graph state instead of tearing it down.
	if (impl) {
		if (impl->device && *impl->device)
			rhi::vulkan::abandon_device(impl->device->Get());
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

org::GraphHost* RenderGraphRuntime::Host()
{
	return IsActive() ? impl->host.get() : nullptr;
}

RenderGraph::BuildManager* RenderGraphRuntime::Builds()
{
	return impl ? impl->builds.get() : nullptr;
}

std::uint64_t RenderGraphRuntime::FlushPendingStreamWork()
{
	if (!IsActive())
		return 0;
	// A stage whose commit point this is commits before the wait.
	FrameAnchors::BeforeGpuWork("a wait for the GPU");
	// So do the epochs still on the submission thread.
	DrainSubmissions("a wait for the GPU");
	const uint64_t enqueued = impl->streamEnqueued.load(std::memory_order_acquire);
	const uint64_t submitted = impl->streamSubmitted.load(std::memory_order_acquire);
	if (enqueued <= submitted)
		return 0;
	if (auto* context = globals::d3d::context)
		context->Flush();
	impl->NoteStreamClosed("flush before a wait", "");
	static std::atomic<uint32_t> logged = 0;
	if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
		logger::info("[ORG] flushed DXVK's stream before a render-thread wait: {} graph submissions were still in it (enqueued {}, submitted {})",
			enqueued - submitted, enqueued, submitted);
	return enqueued - submitted;
}

std::shared_ptr<rhi::TimelinePtr> RenderGraphRuntime::FrameWaitTimeline() const
{
	return IsActive() ? impl->frameWaitTimeline : nullptr;
}

std::shared_ptr<const void> RenderGraphRuntime::DeviceOwner() const
{
	return IsActive() ? impl->device : std::shared_ptr<const void>{};
}

std::uint64_t RenderGraphRuntime::TakeSubmittedSegments()
{
	return impl ? std::exchange(impl->submittedSegments, std::uint64_t{ 0 }) : 0;
}

void RenderGraphRuntime::PollEpochFailures()
{
	if (!IsActive())
		return;
	try {
		(void)impl->host->PollEpochFailures();
	} catch (const std::exception& e) {
		logger::error("[ORG] an epoch failure's handler failed: {}", e.what());
	}
}

bool RenderGraphRuntime::GuardedBuild(const std::function<bool()>& a_build)
{
	FrameAnchors::BeforeGpuWork("a build point");
	try {
		return a_build();
	} catch (const std::exception& e) {
		// The host's thread failed or a build threw. Nothing of the graph runs from here.
		logger::error("[ORG] Render graph build failed, disabling the graph: {}", e.what());
		impl->faulted = true;
		disabledReason = e.what();
		FaultIsFatal(fmt::format("a build failed ({})", e.what()));
		return false;
	}
}

bool RenderGraphRuntime::BuildIfRequested(std::string_view a_caller)
{
	if (!IsActive())
		return false;
	++buildPoints;
	return GuardedBuild([&] { return impl->builds->Poll(a_caller); });
}

bool RenderGraphRuntime::BuildsSettled() const
{
	return !impl || !impl->builds || impl->builds->Settled();
}

bool RenderGraphRuntime::RebuildRequestedUnstaged() const
{
	if (!impl)
		return false;
	if (impl->builds)
		return impl->builds->UnstagedRebuildRequested();
	auto* host = const_cast<RenderGraphRuntime*>(this)->Host();
	return host && host->RebuildRequested();
}

bool RenderGraphRuntime::BuildSetupGraphs(std::string_view a_caller)
{
	if (!IsActive())
		return false;
	++buildPoints;
	return GuardedBuild([&] { return impl->builds->BuildSetupGraphs(a_caller); });
}

void RenderGraphRuntime::OnEpochFailed(std::uint32_t a_epoch, bool a_keptGraph, const std::string& a_error)
{
	// Epochs are segment values, so a failure outside them is the whole graph's.
	if (a_epoch >= static_cast<std::uint32_t>(Segment::Count)) {
		logger::error("[ORG] Render graph failed, disabling the graph: {}", a_error);
		impl->faulted = true;
		disabledReason = a_error;
		FaultIsFatal(a_error);
		return;
	}
	const auto epochSegment = static_cast<Segment>(a_epoch);
	// Graph mode does not drop a failed epoch's clients or hand its work back to the engine.
	FaultIsFatal(fmt::format("{} failed ({})", Impl::SegmentLabel(epochSegment), a_error));
	logger::error("[ORG] {} failed ({}); the other epochs run on: {}", Impl::SegmentLabel(epochSegment),
		a_keptGraph ? "its previous graph still runs" : "it runs nothing until it is built again", a_error);
	// The frame anchors drop the clients of the stage's failed build and build the stage again without them.
	if (FrameAnchors::NamesOf(a_epoch)) {
		FrameAnchors::OnEpochFailed(a_epoch, a_error, a_keptGraph);
		return;
	}
	// A fragment source's epoch goes to its source.
	if (RenderGraph::Frame::Revision::FragmentEpochFailed(a_epoch, Impl::SegmentLabel(epochSegment), a_error))
		return;
	// Outside graph mode any other epoch now fails ExecuteEpoch, and its feature falls back to D3D11.
}

void RenderGraphRuntime::SignalFramePace(bool a_active)
{
	// The frame's end on the render thread. Shader hot reload's owner point, and where finished D3D11 releases are freed.
	ServicePipelineReload();
	ServiceD3D11Releases();
	RenderGraph::Engine::Tripwire::ServiceRetired();
	// Epoch failures go to their owners at the next adoption point (Frame::Driver::OnBegin), not here.
	if (!IsActive() || !impl->framePace || !a_active)
		return;
	auto& state = *impl;
	// In D3D11 stream order, after every command of the frame: the GPU signals it once the frame's work is done.
	const VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, state.framePace, state.framePaceValue + 1,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
	DxvkOrgInteropSubmission submission{};
	submission.version = DXVK_ORG_INTEROP_VERSION;
	submission.signalCount = 1;
	submission.signals = &signal;
	submission.onSubmitted = &Impl::OnStreamSubmitted;
	submission.user = impl.get();
	submission.label = "CS ORG: frame pace";
	state.streamEnqueued.fetch_add(1, std::memory_order_acq_rel);
	if (FAILED(state.enqueueSubmission(globals::d3d::device, &submission))) {
		state.streamEnqueued.fetch_sub(1, std::memory_order_acq_rel);
		return;
	}
	++state.framePaceValue;
	state.framePaceSignalled = true;
}

void RenderGraphRuntime::OnRendererBegin()
{
	const auto frame = RenderGraph::FrameClock::clock.Begin();
	if (FrameAnchors::IsRenderOwner())
		Get().NoteFrameBegin(frame);
}

void RenderGraphRuntime::NoteGpuCompleted(std::uint64_t a_frame)
{
	RenderGraph::FrameClock::clock.NoteGpuCompleted(a_frame);
}

void RenderGraphRuntime::NoteFrameBegin(std::uint64_t a_frame)
{
	if (!impl)
		return;
	auto& state = *impl;
	const auto completed = RenderGraph::FrameClock::GpuCompleted();
	if (!completed)
		return;
	const auto ahead = a_frame > completed ? a_frame - completed : 0;
	++state.aheadCounts[(std::min<std::uint64_t>)(ahead, state.aheadCounts.size() - 1)];
	state.aheadPeak = (std::max)(state.aheadPeak, ahead);
	state.aheadPeakSession = (std::max)(state.aheadPeakSession, ahead);
	if (++state.aheadFrames < 600)
		return;
	std::string counts;
	for (std::size_t i = 0; i < state.aheadCounts.size(); ++i)
		if (state.aheadCounts[i])
			counts += fmt::format("{}{}{}: {}", counts.empty() ? "" : ", ", i, i + 1 == state.aheadCounts.size() ? "+" : "", state.aheadCounts[i]);
	logger::info("[ORG] pacing: Begin ahead of the GPU's last completed epoch, last {} frames: peak {} frames (session {}), by distance {}; published copies "
				 "per frame slot {}",
		state.aheadFrames, state.aheadPeak, state.aheadPeakSession, counts, kGameFramesInFlight);
	state.aheadCounts.fill(0);
	state.aheadPeak = 0;
	state.aheadFrames = 0;
}

void RenderGraphRuntime::SealPresented()
{
	// Before the seal.
	FrameAnchors::BeforeGpuWork("the frame's end");
	// What a site latches from here on goes to the next present.
	RenderGraph::FrameClock::clock.SealPresent();
	if (impl && impl->host)
		impl->host->SealSubmitted();
	// Every stage committed and the present sent the frame's work on, so this release is outside every stage window.
	if (impl) {
		auto released = std::make_shared<std::vector<std::shared_ptr<const void>>>();
		{
			const std::lock_guard lock(impl->frameEndLock);
			released->swap(impl->frameEndReleases);
		}
		if (!released->empty())
			ReleaseAfterD3D11(std::move(released));
	}
	// Native mode bring-up. Logs the host's runs in flight every 600 presents, so a run that never retires shows early.
	if (impl && impl->native && impl->host) {
		static std::uint64_t s_presents = 0;
		// Once: what DCLF's frame values need (FrameValues::Available), the frame-wait timeline and a streaming queue of the uploader's own.
		if (s_presents == 0) {
			const auto uploads = impl->host->RetainUploads();
			logger::info("[ORG native] frame values: frame-wait timeline {}, the uploader's own streaming queue {}", impl->frameWaitTimeline != nullptr,
				uploads && uploads->HasDedicatedStreamingQueue());
		}
		if (++s_presents % 600 == 0)
			logger::info("[ORG native] host state after {} presents: {}", s_presents, impl->host->DescribeAsyncState());
	}
	// The build point for frames DCLF does not run. Render owner only, since a loading screen's thread presents too.
	// DCLF's finished builds are staged here like at any poll. They attach with a snapshot at a Begin.
	if (impl && impl->builds && IsActive() && FrameAnchors::IsRenderOwner()) {
		if (buildPoints == buildPointsAtSeal)
			++buildPoints;
		(void)GuardedBuild([&] { return impl->builds->Poll("frame end"); });
		buildPointsAtSeal = buildPoints;
		impl->builds->NoteFrameEnd();
	}
}

void RenderGraphRuntime::DrainSubmissions(const char* a_why, bool a_anyThread)
{
	if (!impl || !impl->host || !impl->host->SubmissionThreadOn())
		return;
	if (!a_anyThread && !FrameAnchors::IsRenderOwner())
		return;
	impl->host->DrainSubmissions(a_why);
}

bool RenderGraphRuntime::QueuesPresent() const
{
	return impl && impl->host && impl->host->SubmissionThreadOn() && FrameAnchors::IsRenderOwner();
}

void RenderGraphRuntime::QueueOnSubmitThread(std::function<void()> a_closure)
{
	if (!impl || !impl->host) {
		a_closure();
		return;
	}
	impl->host->EnqueueOnSubmitThread(std::move(a_closure));
}

void RenderGraphRuntime::WaitBeforePresent(std::function<void()> a_wait)
{
	if (!impl || !impl->host) {
		a_wait();
		return;
	}
	impl->presentWaits.push_back(std::move(a_wait));
}

std::vector<std::function<void()>> RenderGraphRuntime::TakePresentWaits()
{
	return impl ? std::exchange(impl->presentWaits, {}) : std::vector<std::function<void()>>{};
}

void RenderGraphRuntime::MoveOwnerThread()
{
	// The host needs no other move. ExecuteEpoch takes the calling thread as DXVK's stream thread at each epoch.
	if (impl && impl->host)
		if (auto* uploads = impl->host->Uploads())
			uploads->SetOwnerThread();
}

void RenderGraphRuntime::WaitFramePace()
{
	if (!impl || !std::exchange(impl->framePaceSignalled, false) || !IsActive())
		return;
	auto& state = *impl;
	// Its signal was enqueued before the present, which flushed it to the queue.
	const uint64_t lag = state.frameLatency - 1;
	if (state.framePaceValue <= lag)
		return;
	const uint64_t target = state.framePaceValue - lag;
	ZoneScopedN("CS.ORG.FramePace");
	const auto start = std::chrono::steady_clock::now();
	VkSemaphoreWaitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
	wait.semaphoreCount = 1;
	wait.pSemaphores = &state.framePace;
	wait.pValues = &target;
	// Bounded so a lost signal cannot hang the game. The next frame waits again.
	if (state.waitSemaphores(state.vkDevice, &wait, 0) != VK_TIMEOUT)
		return;
	(void)state.waitSemaphores(state.vkDevice, &wait, 250'000'000ull);
	++state.framePaceWaits;
	state.framePaceWaited += static_cast<uint64_t>(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
	if (state.framePaceWaits % 3000 == 0)
		logger::info("[ORG] frame pacing: {} waits so far, {:.2f} ms average", state.framePaceWaits,
			static_cast<double>(state.framePaceWaited) / 1000.0 / static_cast<double>(state.framePaceWaits));
}

std::uint32_t RenderGraphRuntime::EpochOf(Segment a_segment)
{
	return static_cast<std::uint32_t>(a_segment);
}

void RenderGraphRuntime::ConfigureFrame(FrameConfig a_config)
{
	auto& runtime = Get();
	runtime.frameConfig = std::move(a_config);
	runtime.frameConfigured = true;
}

namespace
{
	// SetEpochHooks. Work hosted in another module's epoch, for every submission path.
	std::atomic<void (*)(RenderGraphRuntime::Segment)> epochStartingHook{ nullptr };
	std::atomic<void (*)(RenderGraphRuntime::Segment)> epochCommitHook{ nullptr };
}

std::shared_ptr<org::ExternalEpochExecution> RenderGraphRuntime::ExecuteExternalEpoch(Segment a_segment,
	const std::function<void(org::RenderGraph&)>& a_beforePrepare, std::shared_ptr<const void> a_resourceOwner)
{
	ZoneScopedN("CS.ORG.ExecuteExternalEpoch");
	if (!IsActive())
		return nullptr;
	// As in ExecuteEpoch, the hosted work sees the submission first and its commit writes the run's data first.
	if (const auto starting = epochStartingHook.load(std::memory_order_acquire))
		starting(a_segment);
	FrameAnchors::BeforeGpuWork("another epoch", static_cast<std::uint32_t>(a_segment));
	impl->CloseLeftOpenExternal();
	impl->TakeCompletedEpochs();
	std::function<void(org::RenderGraph&)> beforePrepare;
	if (a_beforePrepare) {
		beforePrepare = [&](org::RenderGraph& a_graph) {
			ScopedPerfEvent inputsEvent(Impl::InputsLabel(a_segment));
			if (const auto commit = epochCommitHook.load(std::memory_order_acquire))
				commit(a_segment);
			a_beforePrepare(a_graph);
		};
	}
	const char* label = Impl::SegmentLabel(a_segment);
	RenderGraph::UndeclaredAccess::EpochScope undeclaredScope(label);
	try {
		impl->host->SubmitEpoch(EpochOf(a_segment), beforePrepare, std::move(a_resourceOwner));
	} catch (...) {
		// The ticket may have submitted its compute work before the throw. Its execution, if the host made one, is signalled
		// unexecuted, and the stream's exit wait goes in as a submission of its own.
		if (auto lost = impl->host->TakeExternalExecution()) {
			try {
				SignalExecutionUnexecuted(*lost, 0, label);
			} catch (const std::exception& e) {
				logger::error("[ORG] {}: an execution nobody took could not be signalled: {}", label, e.what());
			}
		}
		(void)impl->CloseComputeEpoch(label);
		throw;
	}
	impl->MarkEpoch(impl->host->LastHostFrame(), a_segment);
	auto execution = impl->host->TakeExternalExecution();
	// Open until the caller emits the execution and calls CloseExternalEpoch.
	impl->externalOpen = true;
	impl->externalExitSplit = execution && execution->exitSplit;
	impl->externalLabel = label;
	impl->RecordStreamEvent(execution ? "external epoch" : "external epoch without an execution", label, execution ? execution->segments.size() : 0);
	return execution;
}

void RenderGraphRuntime::CloseExternalEpoch(bool a_emitted)
{
	if (!IsActive() || !impl->externalOpen)
		return;
	impl->externalOpen = false;
	impl->RecordStreamEvent(a_emitted ? "external epoch closed" : "external epoch closed, not emitted", impl->externalLabel);
	if (!impl->CloseComputeEpoch(impl->externalLabel, a_emitted, a_emitted && impl->externalExitSplit))
		logger::error("[ORG] {}: DXVK rejected the compute queue's exit wait", impl->externalLabel);
}

void RenderGraphRuntime::SignalExecutionUnexecuted(const org::ExternalEpochExecution& a_execution, std::size_t a_first, const char* a_label)
{
	if (!IsActive())
		return;
	for (std::size_t index = a_first; index < a_execution.segments.size(); ++index) {
		thread_local std::vector<ExternalCommandsWait> waits, signals;
		waits.clear();
		signals.clear();
		for (const auto& point : a_execution.segments[index].waits)
			waits.push_back(NativeTimelinePoint(point));
		for (const auto& point : a_execution.segments[index].signals)
			signals.push_back(NativeTimelinePoint(point));
		std::scoped_lock lock(impl->streamOrderMutex);
		impl->RecordStreamEventLocked<ExternalCommandsWait, ExternalCommandsWait>("segment signalled unexecuted", a_label, index, waits, signals);
	}
	impl->host->SignalExternalExecutionUnexecuted(a_execution, a_first);
}

void RenderGraphRuntime::SplitStream()
{
	if (!IsActive())
		return;
	globals::d3d::context->Flush();
	impl->NoteStreamClosed("segment split", "");
}

std::string RenderGraphRuntime::DescribeStreamOrder(std::size_t a_events) const
{
	return IsActive() ? impl->DescribeStreamOrder(a_events) : std::string("inactive");
}

bool RenderGraphRuntime::ExecuteEpoch(Segment a_segment, const std::function<void(org::RenderGraph&)>& a_beforePrepare,
	std::shared_ptr<const void> a_resourceOwner)
{
	return TryExecuteEpoch(a_segment, a_beforePrepare, std::move(a_resourceOwner)) == EpochResult::Submitted;
}

void RenderGraphRuntime::SetEpochHooks(EpochHooks a_hooks)
{
	epochStartingHook.store(a_hooks.starting, std::memory_order_release);
	epochCommitHook.store(a_hooks.commit, std::memory_order_release);
}

RenderGraphRuntime::EpochResult RenderGraphRuntime::TryExecuteEpoch(Segment a_segment, const std::function<void(org::RenderGraph&)>& a_beforePrepare,
	std::shared_ptr<const void> a_resourceOwner)
{
	ZoneScopedN("CS.ORG.ExecuteEpoch");
	if (!IsActive())
		return EpochResult::Stopped;
	if (const auto starting = epochStartingHook.load(std::memory_order_acquire))
		starting(a_segment);
	// Stages whose commit point this epoch is commit first.
	FrameAnchors::BeforeGpuWork("another epoch", static_cast<std::uint32_t>(a_segment));
	impl->CloseLeftOpenExternal();
	// Read by passes while the frame prepares and records, all before ExecuteFrame returns.
	segment = a_segment;
	ZoneText(Impl::SegmentLabel(a_segment), std::strlen(Impl::SegmentLabel(a_segment)));
	// Undeclared accesses made meanwhile on this thread (the commits, the inputs) name this epoch.
	RenderGraph::UndeclaredAccess::EpochScope undeclaredScope(Impl::SegmentLabel(a_segment));
	// The epoch's CPU side in capture tools. Its submissions carry a queue label of the same name.
	ScopedPerfEvent epochEvent(Impl::SegmentLabel(a_segment));
	const auto start = std::chrono::steady_clock::now();
	if (!impl->native) {
		// The graph's batches land in DXVK's stream between the D3D11 work issued before and after. Entry and exit barriers order the memory.
		impl->streamThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
	}
	impl->TakeCompletedEpochs();
	const auto completed = std::chrono::steady_clock::now();
	impl->batching = impl->enqueueSubmission && impl->enqueueSubmissions;
	impl->pendingCount = 0;
	try {
		// An epoch with no graph builds at its submission on this thread. That is logged after the first world frame.
		if (!impl->host->Graph(EpochOf(a_segment)) && !impl->host->EpochFailed(EpochOf(a_segment)))
			impl->builds->WarnSynchronous(fmt::format("{} has no graph and is built at its submission", Impl::SegmentLabel(a_segment)), "ExecuteEpoch");
		std::function<void(org::RenderGraph&)> beforePrepare;
		if (a_beforePrepare) {
			beforePrepare = [&](org::RenderGraph& a_graph) {
				ScopedPerfEvent inputsEvent(Impl::InputsLabel(a_segment));
				// Work other modules keep in this epoch's graph writes its run's data first (SetEpochHooks).
				if (const auto commit = epochCommitHook.load(std::memory_order_acquire))
					commit(a_segment);
				a_beforePrepare(a_graph);
			};
		}
		impl->host->SubmitEpoch(EpochOf(a_segment), beforePrepare, a_resourceOwner);
		impl->MarkEpoch(impl->host->LastHostFrame(), a_segment);
		// An in-stream epoch: its graphics work into DXVK's own command stream, here.
		const auto execution = impl->host->TakeExternalExecution();
		if (execution && !impl->EmitExecution(execution, Impl::SegmentLabel(a_segment)))
			throw std::runtime_error("DXVK took no external commands for an in-stream epoch");
		if (!impl->CloseComputeEpoch(Impl::SegmentLabel(a_segment), execution != nullptr, execution && execution->exitSplit))
			throw std::runtime_error("DXVK rejected the compute queue's exit wait");
		if (impl->batching && !impl->FlushPendingSubmits(Impl::SegmentLabel(a_segment)))
			throw std::runtime_error("DXVK rejected the epoch's submissions");
		const auto elapsed = std::chrono::steady_clock::now() - start;
		{
			// Since ExecuteEpoch's last lap (the completions): the host's call and the stream's close.
			const double totalUs = std::chrono::duration<double, std::micro>(elapsed).count();
			const auto& t = impl->host->LastFrameTimings();
			const double sinceCompletions = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - completed).count();
			const double hostUs = t.buildUs + t.ticketWaitUs + t.releaseUs + t.beforePrepareUs + t.checkUs + t.uploadsUs + t.submitUs + t.postUs;
			const double closeUs = (std::max)(0.0, sinceCompletions - hostUs);
			const double inputsUs = t.beforePrepareUs;
			const double submitUs = t.ticketWaitUs + t.submitUs + closeUs;
			epochTotals.inputsUs += inputsUs;
			epochTotals.submitUs += submitUs;
			epochTotals.overheadUs += (std::max)(0.0, totalUs - inputsUs - submitUs);
			++epochTotals.epochs;
		}
		impl->submittedSegments |= std::uint64_t{ 1 } << static_cast<std::uint32_t>(a_segment);
		impl->failedEpochsLogged &= ~(std::uint64_t{ 1 } << static_cast<std::uint32_t>(a_segment));
		return EpochResult::Submitted;
	} catch (const org::GraphHost::EpochCopiesBusyError& e) {
		// A revision-driven epoch ran more often since Present than it has published copies.
		// Nothing was submitted or failed, so the caller falls back for this run.
		(void)impl->CloseComputeEpoch(Impl::SegmentLabel(a_segment));
		if (impl->batching)
			(void)impl->FlushPendingSubmits(Impl::SegmentLabel(a_segment));
		if (const std::uint64_t bit = std::uint64_t{ 1 } << static_cast<std::uint32_t>(a_segment); !(impl->copiesBusyLogged & bit)) {
			impl->copiesBusyLogged |= bit;
			logger::info("[ORG] {} ran more than its {} published copies in one frame; runs past them fall back until the next Present", Impl::SegmentLabel(a_segment),
				e.copies);
		}
		return EpochResult::Skipped;
	} catch (const org::GraphHost::EpochFailedError& e) {
		// Only this epoch's graph failed and nothing was submitted. Fatal in graph mode. Otherwise its owner hears at the next
		// PollEpochFailures, and the caller falls back until then.
		FaultIsFatal(fmt::format("{} did not run, its graph failed ({})", Impl::SegmentLabel(a_segment), e.what()));
		(void)impl->CloseComputeEpoch(Impl::SegmentLabel(a_segment));
		if (impl->batching)
			(void)impl->FlushPendingSubmits(Impl::SegmentLabel(a_segment));
		if (const std::uint64_t bit = std::uint64_t{ 1 } << static_cast<std::uint32_t>(a_segment); !(impl->failedEpochsLogged & bit)) {
			impl->failedEpochsLogged |= bit;
			logger::error("[ORG] {} did not run, its graph failed (the other epochs run on): {}", Impl::SegmentLabel(a_segment), e.what());
		}
		return EpochResult::EpochFailed;
	} catch (const std::exception& e) {
		// What the graph committed still goes to DXVK in order, then the graph stops. An untaken execution is signalled
		// unexecuted first, since compute work may wait for its segments.
		if (auto lost = impl->host->TakeExternalExecution()) {
			try {
				SignalExecutionUnexecuted(*lost, 0, Impl::SegmentLabel(a_segment));
			} catch (const std::exception& signalError) {
				logger::error("[ORG] {}: an execution nobody took could not be signalled: {}", Impl::SegmentLabel(a_segment), signalError.what());
			}
		}
		(void)impl->CloseComputeEpoch(Impl::SegmentLabel(a_segment));
		if (impl->batching)
			(void)impl->FlushPendingSubmits(Impl::SegmentLabel(a_segment));
		// Fatal in graph mode. Outside it the graph stops and callers fall back to D3D11.
		logger::error("[ORG] Render graph epoch failed, disabling the graph: {}", e.what());
		// What the host held when it failed: its runs in flight, what each waits for, and the timelines (read racily, for the report).
		if (impl->host)
			logger::error("[ORG] host state at the failure: {}", impl->host->DescribeAsyncState());
		impl->faulted = true;
		disabledReason = e.what();
		FaultIsFatal(fmt::format("{} failed ({})", Impl::SegmentLabel(a_segment), e.what()));
		return EpochResult::Stopped;
	}
}

void RenderGraphRuntime::ReportGpuTimings(std::uint32_t a_frames, bool a_log)
{
	if (!impl || !a_frames)
		return;
	impl->TakeCompletedEpochs();
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
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
	return impl ? impl->shaderCompiler.get() : nullptr;
#else
	return nullptr;
#endif
}

org::services::PipelineRegistry* RenderGraphRuntime::PipelineRegistry()
{
	// Never destroyed: a rebuild may complete while the process tears down (Shutdown releases what it holds of the device's).
	static auto* registry = new org::services::PipelineRegistry;
	return registry;
}

void RenderGraphRuntime::RequestTimestampDump(std::uint32_t a_epochs)
{
	timestampDumps.store(static_cast<int>(a_epochs), std::memory_order_relaxed);
}

void RenderGraphRuntime::RequestShaderReload(std::string_view a_changedFile, bool a_force)
{
	logger::info("[ORG] shader reload asked: {}{}", a_changedFile.empty() ? std::string("every live pipeline") : fmt::format("the live pipelines built from {}", a_changedFile),
		a_force ? ", each swapped even if unchanged" : "");
	PipelineRegistry()->RequestReload(a_changedFile, a_force);
}

void RenderGraphRuntime::ServicePipelineReload()
{
	auto& registry = *PipelineRegistry();
	const auto result = registry.Service();
	if (result.finished) {
		const auto outcome = registry.LastReload();
		logger::info("[ORG] shader reload: {} of {} pipelines rebuilt ({} failed, {} unchanged) in {:.2f} s off the render thread; {} new versions at pipeline "
					 "generation {}",
			outcome.rebuilt, outcome.entries, outcome.failed, outcome.unchanged, outcome.seconds, result.published, outcome.generation);
		for (const auto& error : outcome.errors)
			logger::error("[ORG] shader reload: {} (it keeps its pipeline)", error.substr(0, 2000));
	}
	if (result.published)
		pipelineGenerationPublished = (std::max)(pipelineGenerationPublished, registry.Generation());
	// The new versions are an event: every graph is built again off the render thread, and the revision that carries the new
	// ShaderGeneration adopts them once every graph is at it (RenderGraph/PipelineVersions.h). Frames keep the old versions meanwhile.
	// Render owner only: a loading screen's thread presents too, and the adopted values are the render thread's.
	if (pipelineGenerationPublished > pipelineGenerationRaised && impl && impl->builds && FrameAnchors::IsRenderOwner()) {
		pipelineGenerationRaised = pipelineGenerationPublished;
		const auto why = fmt::format("shader reload {}: pipeline generation {}", registry.LastReload().serial, pipelineGenerationRaised);
		RenderGraph::Builds::Wanted wanted;
		wanted.all = true;
		RenderGraph::Builds::Ask(std::move(wanted), why);
		(void)RenderGraph::Frame::Vars::Set(RenderGraph::Frame::Vars::ShaderGeneration(), pipelineGenerationRaised, why);
		logger::info("[ORG] {}: every graph is built again off the render thread; the revision that carries it adopts them", why);
	} else if (result.published && impl && impl->host && !impl->builds) {
		// No build manager: the host builds again at its next build point, and those builds pin the new versions.
		impl->host->RequestRebuild("shader reload: new pipeline versions");
	}
	// The versions no graph or kept recording pins any more go once the retirement clock passes everything submitted so far.
	if (impl && impl->host) {
		registry.Release(RenderGraph::Frame::Retirement::SubmittedPoint());
		registry.Retire(RenderGraph::Frame::Retirement::Clock()->Completed());
	} else {
		registry.Release(0);
		registry.Retire(~0ull);
	}
}

const std::vector<std::filesystem::path>& RenderGraphRuntime::ShaderSourceFiles()
{
	// Scanned once, and again after each shader reload that added or removed files. The reference stays valid until this thread's next call.
	static std::mutex mutex;
	static std::shared_ptr<const std::vector<std::filesystem::path>> current;
	static std::uint64_t scannedFor = ~0ull;
	thread_local std::shared_ptr<const std::vector<std::filesystem::path>> held;
	const std::uint64_t generation = PipelineRegistry()->SourcesGeneration();
	std::scoped_lock lock(mutex);
	if (!current || scannedFor != generation) {
		auto found = std::make_shared<std::vector<std::filesystem::path>>();
		std::error_code ec;
		for (auto it = std::filesystem::recursive_directory_iterator(kShaderDirectory, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
			const auto extension = it->path().extension();
			if (it->is_regular_file() && (extension == ".hlsl" || extension == ".hlsli"))
				found->push_back(it->path());
		}
		current = std::move(found);
		scannedFor = generation;
	}
	held = current;
	return *held;
}

bool RenderGraphRuntime::DescribeResource(IUnknown* a_object, DxvkOrgInteropResourceInfo& a_info)
{
	// Native mode: the images its device made (the back buffer, the native textures), described as DXVK would describe its own.
	if (impl && impl->native)
		return RenderGraph::Engine::Native::DescribeResource(a_object, a_info);
	// An engine field may hold a tripwire handle (CS_ORG_ENGINE_TARGET_HANDLES): DXVK describes the object it wraps.
	a_object = RenderGraph::Engine::Tripwire::UnwrapObject(a_object);
	if (!impl || !a_object)
		return false;
	a_info = {};
	a_info.version = DXVK_ORG_INTEROP_VERSION;
	const bool described = SUCCEEDED(impl->getResourceInfo(globals::d3d::device, a_object, &a_info));
	return described;
}

namespace
{
	/** @brief Unwraps tripwire handles in a batch for DXVK. Copies into a_scratch only when the batch holds a handle. */
	std::span<IUnknown* const> UnwrapBatch(std::span<IUnknown* const> a_objects, std::vector<IUnknown*>& a_scratch)
	{
		using RenderGraph::Engine::Tripwire::IsHandle;
		if (std::none_of(a_objects.begin(), a_objects.end(), [](IUnknown* a_object) { return a_object && IsHandle(a_object); }))
			return a_objects;
		a_scratch.assign(a_objects.begin(), a_objects.end());
		for (auto& object : a_scratch)
			object = RenderGraph::Engine::Tripwire::UnwrapObject(object);
		return a_scratch;
	}
}

bool RenderGraphRuntime::DescribeResources(std::span<IUnknown* const> a_objects, std::span<DxvkOrgInteropResourceInfo> a_infos, std::span<HRESULT> a_results)
{
	// Native mode: one at a time, from native mode's own records (no DXVK to ask).
	if (impl && impl->native) {
		if (a_infos.size() != a_objects.size() || a_results.size() != a_objects.size())
			return false;
		for (std::size_t i = 0; i < a_objects.size(); ++i)
			a_results[i] = RenderGraph::Engine::Native::DescribeResource(a_objects[i], a_infos[i]) ? S_OK : E_INVALIDARG;
		return true;
	}
	std::vector<IUnknown*> scratch;
	a_objects = UnwrapBatch(a_objects, scratch);
	if (!impl || a_infos.size() != a_objects.size() || a_results.size() != a_objects.size())
		return false;
	for (auto& info : a_infos) {
		info = {};
		info.version = DXVK_ORG_INTEROP_VERSION;
	}
	const bool described = SUCCEEDED(impl->getResourceInfos(globals::d3d::device, static_cast<UINT>(a_objects.size()), a_objects.data(), a_infos.data(), a_results.data()));
	return described;
}

bool RenderGraphRuntime::PinlessImports() const
{
	return impl && impl->trackResources;
}

bool RenderGraphRuntime::TrackResources(std::span<IUnknown* const> a_objects, std::span<DxvkOrgInteropTrackedResource> a_resources, std::span<HRESULT> a_results)
{
	std::vector<IUnknown*> scratch;
	a_objects = UnwrapBatch(a_objects, scratch);
	if (!PinlessImports() || a_resources.size() != a_objects.size() || a_results.size() != a_objects.size())
		return false;
	for (auto& resource : a_resources) {
		resource = {};
		resource.version = DXVK_ORG_TRACKED_RESOURCE_VERSION;
		resource.info.version = DXVK_ORG_INTEROP_VERSION;
	}
	const bool described = SUCCEEDED(impl->trackResources(globals::d3d::device, static_cast<UINT>(a_objects.size()), a_objects.data(), a_resources.data(), a_results.data()));
	return described;
}

void RenderGraphRuntime::UntrackResources(std::span<IUnknown* const> a_objects)
{
	std::vector<IUnknown*> scratch;
	a_objects = UnwrapBatch(a_objects, scratch);
	if (PinlessImports() && !a_objects.empty())
		impl->untrackResources(globals::d3d::device, static_cast<UINT>(a_objects.size()), a_objects.data());
}

std::uint32_t RenderGraphRuntime::PollRelocations(std::span<DxvkOrgInteropRelocation> a_relocations)
{
	std::uint32_t count = 0;
	if (PinlessImports() && !a_relocations.empty() &&
		FAILED(impl->pollRelocations(globals::d3d::device, a_relocations.data(), static_cast<std::uint32_t>(a_relocations.size()), &count)))
		count = 0;
	return count;
}

void RenderGraphRuntime::ReleaseStorage(std::span<void* const> a_storages)
{
	if (PinlessImports() && !a_storages.empty())
		impl->releaseStorage(globals::d3d::device, static_cast<UINT>(a_storages.size()), a_storages.data());
}

bool RenderGraphRuntime::EmitExternalCommands(void (*a_record)(void* a_user, VkCommandBuffer a_commandBuffer), void* a_user,
	std::span<const ExternalCommandsWait> a_waits, std::span<const ExternalCommandsWait> a_signals, bool a_bindsState, const char* a_label,
	bool a_fullBarrier)
{
	if (!impl || !a_record)
		return false;
	// Native mode takes no Vulkan command buffers. EmitCommands records BasicRHI commands, and epoch work records inline at execution.
	if (impl->native) {
		static std::atomic_flag logged;
		if (!logged.test_and_set())
			logger::error(
				"[ORG] native: EmitExternalCommands ('{}') takes Vulkan command buffers, which native mode has none of; use EmitCommands "
				"(logged once)",
				a_label ? a_label : "");
		return false;
	}
	// Stages whose commit point this is commit first.
	FrameAnchors::BeforeGpuWork("external commands");
	// DXVK copies the semaphores at the call: one scratch per calling thread (the render thread), reused.
	thread_local std::vector<VkSemaphoreSubmitInfo> semaphores;
	semaphores.clear();
	for (const auto points : { a_waits, a_signals }) {
		for (const auto& point : points) {
			VkSemaphoreSubmitInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
			info.semaphore = point.semaphore;
			info.value = point.value;
			info.stageMask = point.stages;
			semaphores.push_back(info);
		}
	}
	DxvkOrgInteropExternalCommands commands{};
	commands.version = DXVK_ORG_EXTERNAL_COMMANDS_VERSION;
	commands.flags = (a_bindsState ? DXVK_ORG_EXTERNAL_COMMANDS_BINDS_STATE : 0u) | (a_fullBarrier ? DXVK_ORG_EXTERNAL_COMMANDS_FULL_BARRIER : 0u);
	commands.callback = a_record;
	commands.user = a_user;
	commands.waitCount = static_cast<std::uint32_t>(a_waits.size());
	commands.waits = semaphores.data();
	commands.signalCount = static_cast<std::uint32_t>(a_signals.size());
	commands.signals = semaphores.data() + a_waits.size();
	commands.label = a_label;
	// The guard may flush, and DXVK's worker sees that before these commands. The scratch above is unaffected.
	impl->GuardStreamWaits(a_waits, a_label);
	if (FAILED(impl->emitExternalCommands(globals::d3d::device, &commands)))
		return false;
	impl->NoteExternalCommands(a_label, a_waits, a_signals);
	return true;
}

RenderGraphRuntime::ExternalCommandsWait RenderGraphRuntime::NativeTimelinePoint(const rhi::TimelinePoint& a_point)
{
	const auto& runtime = Get();
	rhi::Timeline object(a_point.t);
	if (runtime.IsActive() && runtime.impl->nativeTimelines) {
		object.impl = runtime.impl->nativeTimelines.Get().impl;
		object.vt = runtime.impl->nativeTimelines.Get().vt;
	}
	rhi::VulkanTimelineInfo info{};
	if (!object || !rhi::QueryNativeTimeline(object, rhi::RHI_IID_VK_TIMELINE, &info, sizeof(info)))
		return {};
	return { static_cast<VkSemaphore_T*>(info.timeline), a_point.value, 0 };
}

namespace
{
	/** @brief EmitCommands' recording in DXVK's stream: the client's callback, and the wrapper its commands record through. */
	struct AdoptedCommands
	{
		void (*record)(void*, rhi::CommandList&) = nullptr;
		void* user = nullptr;
		rhi::Device device;
		// Kept until D3D11 work issued after the call completes, since the wrapper owns GPU scratch its recording may allocate.
		std::shared_ptr<rhi::CommandListPtr> wrapper;
	};

	/** @brief DXVK's CS thread, once: DXVK's command buffer adopted as a BasicRHI list for the client's callback. */
	void RecordAdoptedCommands(void* a_user, VkCommandBuffer a_commandBuffer)
	{
		const std::unique_ptr<AdoptedCommands> commands(static_cast<AdoptedCommands*>(a_user));
		*commands->wrapper = rhi::vulkan::AdoptCommandBuffer(commands->device, a_commandBuffer, rhi::QueueKind::Graphics);
		if (!*commands->wrapper) {
			static std::atomic_flag logged;
			if (!logged.test_and_set())
				logger::error("[ORG] EmitCommands: DXVK's command buffer could not be adopted; the caller's commands are left out (logged once)");
			return;
		}
		auto list = commands->wrapper->Get();
		commands->record(commands->user, list);
	}

	void RecordNoCommands(void*, VkCommandBuffer) {}
}

bool RenderGraphRuntime::EmitCommands(void (*a_record)(void* a_user, rhi::CommandList& a_commands), void* a_user, const rhi::TimelinePoint* a_waits,
	std::uint32_t a_waitCount, const rhi::TimelinePoint* a_signals, std::uint32_t a_signalCount, bool a_bindsState, const char* a_label, bool a_fullBarrier)
{
	if (!impl || !impl->host)
		return false;
	if (impl->native) {
		// Stages whose commit point this is commit first.
		FrameAnchors::BeforeGpuWork("commands");
		// And after the epochs still on the submission thread.
		DrainSubmissions("commands");
		return RenderGraph::Engine::Native::EmitCommands(a_record, a_user, a_waits, a_waitCount, a_signals, a_signalCount, a_label);
	}
	// DXVK takes Vulkan semaphores: each point through the host's timelines.
	thread_local std::vector<ExternalCommandsWait> points;
	points.clear();
	for (const auto& point : std::span(a_waits, a_waitCount))
		points.push_back(NativeTimelinePoint(point));
	for (const auto& point : std::span(a_signals, a_signalCount))
		points.push_back(NativeTimelinePoint(point));
	if (std::ranges::any_of(points, [](const ExternalCommandsWait& a_point) { return a_point.semaphore == nullptr; })) {
		static std::atomic_flag logged;
		if (!logged.test_and_set())
			logger::error("[ORG] EmitCommands ('{}'): a wait or signal is not on one of the host's timelines; nothing emitted (logged once)", a_label ? a_label : "");
		return false;
	}
	const std::span<const ExternalCommandsWait> waits(points.data(), a_waitCount), signals(points.data() + a_waitCount, a_signalCount);
	if (!a_record)
		return EmitExternalCommands(&RecordNoCommands, nullptr, waits, signals, false, a_label, a_fullBarrier);
	auto wrapper = std::make_shared<rhi::CommandListPtr>();
	// DXVK's CS thread frees it in the callback, which may run before the call returns; when the call fails, it never runs.
	auto* commands = new AdoptedCommands{ a_record, a_user, impl->host->Device(), wrapper };
	if (!EmitExternalCommands(&RecordAdoptedCommands, commands, waits, signals, a_bindsState, a_label, a_fullBarrier)) {
		delete commands;
		return false;
	}
	// Released at the frame's end behind the frame's work, which covers these commands.
	{
		const std::lock_guard lock(impl->frameEndLock);
		impl->frameEndReleases.push_back(std::move(wrapper));
	}
	return true;
}

bool RenderGraphRuntime::RecordsAtExecution(std::uint32_t a_epoch) const
{
	if (!impl || !impl->host)
		return false;
	return impl->host->InlineAtExecution() || std::ranges::contains(impl->inStreamEpochs, a_epoch);
}

std::vector<std::uint32_t> RenderGraphRuntime::InStreamEpochs() const
{
	return impl ? impl->inStreamEpochs : std::vector<std::uint32_t>{};
}

winrt::com_ptr<ID3D11Buffer> RenderGraphRuntime::WrapBuffer(org::Resource& a_buffer, const D3D11_BUFFER_DESC& a_desc)
{
	winrt::com_ptr<ID3D11Buffer> result;
	if (!IsActive())
		return result;
	// Native mode: a native buffer handle over the graph's buffer.
	if (impl->native)
		return RenderGraph::Engine::Native::WrapBuffer(a_buffer.GetAPIResource(), a_desc, a_buffer.GetName().c_str());
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
	} else {
		// The D3D11 view of the graph's buffer: named after it, so VRAM attribution lists the two together.
		Util::SetResourceName(result.get(), "%s (D3D11)", a_buffer.GetName().c_str());
	}
	return result;
}

winrt::com_ptr<ID3D11Resource> RenderGraphRuntime::WrapTexture(const rhi::Resource& a_image, const DxvkOrgInteropTextureWrap& a_wrap,
	DxvkOrgInteropResourceInfo& a_info, const char* a_name, const D3D11_SUBRESOURCE_DATA* a_initialData, RenderGraph::Engine::Native::WrapFlags a_flags)
{
	winrt::com_ptr<ID3D11Resource> result;
	// Native mode: a native texture handle over the port's image. DXVK's wrap only queues its zero fill, so guest ignores a_flags.
	if (IsActive() && impl->native)
		return RenderGraph::Engine::Native::WrapTexture(a_image, a_wrap, a_info, a_name, a_initialData, a_flags);
	if (!IsActive())
		return result;
	rhi::VulkanResourceInfo info{};
	if (!rhi::vulkan::get_resource_info(a_image, info) || !info.resource) {
		logger::error("[ORG] WrapTexture: '{}' has no Vulkan image", a_name);
		return result;
	}
	auto wrap = a_wrap;
	wrap.version = DXVK_ORG_INTEROP_VERSION;
	wrap.image = rhi::vulkan::from_native_void<VkImage>(info.resource);
	wrap.imageFormat = static_cast<VkFormat>(info.nativeFormat);
	wrap.imageFlags = info.flags;
	wrap.imageUsage = info.usage;
	a_info = {};
	a_info.version = DXVK_ORG_INTEROP_VERSION;
	const HRESULT created = a_initialData ? impl->createTextureFromVkImageWithData(globals::d3d::device, &wrap, a_initialData, result.put(), &a_info) :
	                                        impl->createTextureFromVkImage(globals::d3d::device, &wrap, result.put(), &a_info);
	if (FAILED(created)) {
		logger::error("[ORG] WrapTexture: DXVK rejected '{}' (format {}, usage 0x{:X}, flags 0x{:X}; see DXVK's log)", a_name,
			wrap.format, wrap.imageUsage, wrap.imageFlags);
		result = nullptr;
	} else {
		// The D3D11 view of the graph's image, named after it as WrapBuffer names buffers.
		Util::SetResourceName(result.get(), "%s (D3D11)", a_name);
	}
	return result;
}

namespace
{
	/** @brief An event query ended at this point of D3D11's stream; null, logged once, when D3D11 cannot make one. */
	winrt::com_ptr<ID3D11Query> IssueReleaseQuery()
	{
		D3D11_QUERY_DESC desc{ D3D11_QUERY_EVENT, 0 };
		winrt::com_ptr<ID3D11Query> query;
		if (FAILED(globals::d3d::device->CreateQuery(&desc, query.put()))) {
			static std::atomic_flag logged;
			if (!logged.test_and_set())
				logger::warn("[ORG] a release query could not be made; the release waits for a later frame's");
			return nullptr;
		}
		globals::d3d::context->End(query.get());
		return query;
	}
}

void RenderGraphRuntime::ReleaseAfterD3D11(std::shared_ptr<const void> a_owner)
{
	if (!a_owner)
		return;
	if (!impl)
		return;  // shut down: DXVK's device is gone or going, and nothing of the graph's runs any more
	if (impl->native) {
		impl->nativeReleases.push_back({ impl->host ? impl->host->SubmissionsMade() : 0, RenderGraph::Engine::Native::GraphicsProgress(), std::move(a_owner) });
		ServiceD3D11Releases();
		return;
	}
	// Parked without a query when D3D11 cannot make one now. A later frame issues it, which is still after the release in the stream.
	impl->d3d11Releases.push_back({ IssueReleaseQuery(), std::move(a_owner) });
	ServiceD3D11Releases();
}

void RenderGraphRuntime::ServiceD3D11Releases()
{
	if (!impl)
		return;
	if (impl->native) {
		const std::uint64_t retired = impl->retiredSubmissions.load(std::memory_order_acquire);
		while (!impl->nativeReleases.empty() && impl->nativeReleases.front().submissions <= retired) {
			auto& queue = impl->nativeReleases.front().queue;
			if (queue.timeline && queue.submitted && queue.timeline.GetCompletedValue() < queue.submitted)
				break;
			impl->nativeReleases.pop_front();
		}
		return;
	}
	auto& releases = impl->d3d11Releases;
	// Owners parked without a query get one now, in order: each covers its release and everything issued since.
	for (auto& release : releases) {
		if (release.query)
			continue;
		release.query = IssueReleaseQuery();
		if (!release.query)
			break;
	}
	// In stream order: once one is pending (or still parked), every later one is too.
	while (!releases.empty() && releases.front().query &&
		   globals::d3d::context->GetData(releases.front().query.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
		releases.pop_front();
}


// End-to-end check of the render-graph-on-DXVK path without the game.
//
// Loads the DXVK fork (dxvk_dxgi.dll + dxvk_d3d11.dll), requests the interop device
// features, creates a D3D11 device, and then does what RenderGraphRuntime does in the
// plugin: BasicRHI adopts DXVK's VkDevice and an OpenRenderGraph PersistentGraphHost
// runs a compute pass on it. The pass writes a graph-owned buffer that D3D11 also writes
// (a sentinel, before the epoch) and reads (a copy, after it) through
// dxvkCreateBufferFromVkBuffer, so a correct readback proves the handoff in both directions.
//
// Submission modes:
//   lock   - each epoch flushes DXVK and waits for it, then submits under DXVK's queue lock;
//   stream - no flush: the graph's batches go into DXVK's command stream
//            (dxvkEnqueueInteropSubmission) between the D3D11 commands around the epoch.
//   resources - stream mode with compiler-to-Vulkan manifest assertions.
//   scoped - resources mode with graph boundary barriers disabled; explicitly
//            declared native accesses drive ledger-derived buffer barriers.
// Finally the D3D11 device is released and DXVK's teardown callback must fire before
// its VkDevice is destroyed.
//
// Usage: OrgDxvkInteropTest <directory containing dxvk_d3d11.dll and dxvk_dxgi.dll> [lock|stream|resources|scoped|leased]

#include <rhi_interop_vulkan.h>

#include "Features/Upscaling/DXVKInteropInterfaces.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "ImageHandoffTests.h"

#include <OpenRenderGraph/PersistentGraphHost.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <Render/RenderGraph/ExecutionBoundary.h>
#include <stdexcept>
#include "../../extern/dxvk/src/dxvk/dxvk_external_access.h"
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef ORG_DXVK_TEST_SPV
#	error "ORG_DXVK_TEST_SPV must name the compiled SPIR-V of WriteValues.hlsl"
#endif

using Microsoft::WRL::ComPtr;

#define REQUIRE(cond, msg)                                                              \
	do {                                                                                \
		if (!(cond)) {                                                                  \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);         \
			return 1;                                                                   \
		}                                                                               \
	} while (0)

namespace
{
	constexpr uint32_t kCount = 256;
	constexpr uint64_t kBytes = kCount * sizeof(uint32_t);

	struct Program
	{
		rhi::PipelineLayoutPtr layout;
		rhi::PipelinePtr pipeline;
	};

	struct WriteBindings
	{
		org::ResourceBindingToken target;
	};

	struct WriteFrame
	{
		std::shared_ptr<const Program> program;
		uint32_t targetIndex = 0;
		uint32_t value = 0;
	};

	class WritePass final : public org::TypedRenderGraphPass<WritePass, WriteFrame, WriteBindings>
	{
	public:
		WritePass(std::shared_ptr<org::Buffer> target, std::shared_ptr<const Program> program, const uint32_t* value) :
			target(std::move(target)), program(std::move(program)), value(value) {}
		WriteBindings Declare(org::PassBuilder& builder)
		{
			builder.PreferQueue(org::QueueKind::Graphics);
			return { builder.BindUnorderedAccess(target) };
		}
		WriteFrame Prepare(const WriteBindings& bindings, const org::PassPrepareContext& preparation) const
		{
			return { program, preparation.ResolveView(bindings.target, { org::BindlessViewKind::UnorderedAccess }).index, *value };
		}
		static void Record(const WriteBindings&, const WriteFrame& frame, org::PassRecordContext& recording)
		{
			auto& commands = recording.Commands();
			commands.BindLayout(frame.program->layout->GetHandle());
			commands.BindPipeline(frame.program->pipeline->GetHandle());
			const uint32_t constants[3] = { frame.targetIndex, frame.value, kCount };
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, 3, constants);
			commands.Dispatch((kCount + 63) / 64, 1, 1);
		}

	private:
		std::shared_ptr<org::Buffer> target;
		std::shared_ptr<const Program> program;
		const uint32_t* value;
	};

	class WriteExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		WriteExtension(std::shared_ptr<org::Buffer> target, std::shared_ptr<const Program> program, const uint32_t* value) :
			target(std::move(target)), program(std::move(program)), value(value) {}
		void PrepareForBuild(org::RenderGraph& graph) override
		{
			graph.RegisterResource(org::ResourceIdentifier("test.values"), target);
		}
		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& out) override
		{
			out.push_back(org::RenderGraph::ExternalPassDesc::Compute("test.write-values",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<WritePass>(target, program, value)))
					.PreferQueue(org::QueueKind::Graphics));
		}

	private:
		std::shared_ptr<org::Buffer> target;
		std::shared_ptr<const Program> program;
		const uint32_t* value;
	};

	std::shared_ptr<const Program> LoadProgram(rhi::Device device)
	{
		std::ifstream file(ORG_DXVK_TEST_SPV, std::ios::binary);
		std::vector<char> spirv((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (spirv.empty())
			return {};
		auto program = std::make_shared<Program>();
		rhi::PushConstantRangeDesc constants{};
		constants.visibility = rhi::ShaderStage::Compute;
		constants.num32BitValues = 3;
		if (device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PipelineLayoutFlags::PF_None },
				program->layout) != rhi::Result::Ok)
			return {};
		rhi::SubobjLayout layout{ program->layout->GetHandle() };
		rhi::SubobjShader shader{ rhi::ShaderStage::Compute, { spirv.data(), static_cast<uint32_t>(spirv.size()) }, "main" };
		const rhi::PipelineStreamItem items[] = { rhi::Make(layout), rhi::Make(shader) };
		if (device.CreatePipeline(items, 2, program->pipeline) != rhi::Result::Ok)
			return {};
		return program;
	}

	struct Locking
	{
		IDXGIVkInteropDevice* interop = nullptr;
		int locks = 0;
		int depth = 0;
		bool unbalanced = false;
	};

	void LockQueue(void* user, VkQueue)
	{
		auto* locking = static_cast<Locking*>(user);
		locking->interop->LockSubmissionQueue();
		++locking->locks;
		if (++locking->depth != 1)
			locking->unbalanced = true;
	}

	void UnlockQueue(void* user, VkQueue)
	{
		auto* locking = static_cast<Locking*>(user);
		if (--locking->depth != 0)
			locking->unbalanced = true;
		locking->interop->ReleaseSubmissionQueue();
	}

	struct Streaming
	{
		ID3D11Device* device = nullptr;
		PFN_dxvkEnqueueInteropSubmission enqueue = nullptr;
		std::atomic<int> enqueued = 0;
		std::atomic<int> submitted = 0;
		std::atomic<int> failed = 0;
		std::atomic<int> manifested = 0;
		std::atomic<int> completed = 0;
		DxvkOrgInteropResourceInterface resources{};
		uint64_t lease = 0;
		VkSemaphore gate = VK_NULL_HANDLE;
		uint64_t gateValue = 0;
		PFN_dxvkEnqueueBufferHandoff bufferHandoff = nullptr;
		VkBuffer registeredBuffer = VK_NULL_HANDLE;
		std::vector<DxvkOrgInteropBufferAccess> nativeManifest;
	};

	void OnStreamSubmitted(void* user, VkResult result)
	{
		auto* streaming = static_cast<Streaming*>(user);
		if (result != VK_SUCCESS)
			++streaming->failed;
		++streaming->submitted;
	}

	void OnStreamCompleted(void* user, VkResult result)
	{
		auto& streaming = *static_cast<Streaming*>(user);
		if (result != VK_SUCCESS) ++streaming.failed;
		++streaming.completed;
	}

	// What RenderGraphRuntime's submit hook does on the D3D11 thread.
	VkResult SubmitToStream(void* user, VkQueue, const VkSubmitInfo2& submit)
	{
		auto* streaming = static_cast<Streaming*>(user);
		if (streaming->resources.context) {
			VkSubmitInfo2 owned = submit;
			std::vector<VkSemaphoreSubmitInfo> waits;
			if (submit.waitSemaphoreInfoCount)
				waits.assign(submit.pWaitSemaphoreInfos, submit.pWaitSemaphoreInfos + submit.waitSemaphoreInfoCount);
			VkSemaphoreSubmitInfo gate{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
			gate.semaphore = streaming->gate;
			gate.value = streaming->gateValue;
			gate.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			if (streaming->gate) waits.push_back(gate);
			owned.waitSemaphoreInfoCount = static_cast<uint32_t>(waits.size());
			owned.pWaitSemaphoreInfos = waits.data();
			DxvkOrgInteropLeasedSubmission leased{};
			leased.version = DXVK_ORG_RESOURCE_INTERFACE_VERSION;
			leased.batch.version = DXVK_ORG_INTEROP_VERSION;
			leased.batch.submitCount = 1;
			leased.batch.submits = &owned;
			leased.batch.onSubmitted = &OnStreamSubmitted;
			leased.batch.user = streaming;
			leased.leaseCount = 1;
			leased.leaseTokens = &streaming->lease;
			leased.onCompleted = &OnStreamCompleted;
			leased.completionUser = streaming;
			HRESULT result;
			if (streaming->bufferHandoff) {
				DxvkOrgInteropBufferHandoff handoff{};
				handoff.version = DXVK_ORG_BUFFER_HANDOFF_VERSION;
				handoff.submission = leased;
				handoff.accessCount = static_cast<uint32_t>(streaming->nativeManifest.size());
				handoff.accesses = streaming->nativeManifest.data();
				result = streaming->bufferHandoff(streaming->resources.context, &handoff);
			} else {
				result = streaming->resources.enqueue(streaming->resources.context, &leased);
			}
			if (FAILED(result)) return VK_ERROR_UNKNOWN;
			++streaming->enqueued;
			return VK_SUCCESS;
		}
		DxvkOrgInteropSubmission submission{};
		submission.version = DXVK_ORG_INTEROP_VERSION;
		submission.waitCount = submit.waitSemaphoreInfoCount;
		submission.waits = submit.pWaitSemaphoreInfos;
		submission.commandBufferCount = submit.commandBufferInfoCount;
		submission.commandBuffers = submit.pCommandBufferInfos;
		submission.signalCount = submit.signalSemaphoreInfoCount;
		submission.signals = submit.pSignalSemaphoreInfos;
		submission.onSubmitted = &OnStreamSubmitted;
		submission.user = streaming;
		++streaming->enqueued;
		return SUCCEEDED(streaming->enqueue(streaming->device, &submission)) ? VK_SUCCESS : VK_ERROR_UNKNOWN;
	}


	// Validate the compiler -> concrete backing -> Vulkan manifest path without
	// claiming that the legacy DXVK submission API consumes these dependencies.
	VkResult SubmitResourcesToStream(void* user, VkQueue queue, const VkSubmitInfo2& submit,
		rhi::Span<rhi::vulkan::CommandBufferResourceAccesses> manifests)
	{
		auto* streaming = static_cast<Streaming*>(user);
		if (manifests.size != submit.commandBufferInfoCount)
			return VK_ERROR_VALIDATION_FAILED_EXT;
		streaming->nativeManifest.clear();
		for (uint32_t i = 0; i < manifests.size; ++i) {
			const auto& manifest = manifests.data[i];
			if (manifest.commandBuffer != submit.pCommandBufferInfos[i].commandBuffer)
				return VK_ERROR_VALIDATION_FAILED_EXT;
			if (!manifest.complete) {
				if (streaming->bufferHandoff) return VK_ERROR_VALIDATION_FAILED_EXT;
				continue; // Nongraph upload lists are not yet declared.
			}
			bool targetWrite = false;
			for (const auto& access : manifest.accesses) {
				if (streaming->bufferHandoff) {
					if (access.buffer != streaming->registeredBuffer || access.image)
						return VK_ERROR_VALIDATION_FAILED_EXT;
					streaming->nativeManifest.push_back({streaming->lease, access.offset, access.size, access.stages, access.access});
				}
				if (access.buffer && access.size == kBytes && access.write
					&& (access.stages & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
					&& (access.access & VK_ACCESS_2_SHADER_WRITE_BIT))
					targetWrite = true;
			}
			if (!targetWrite) return VK_ERROR_VALIDATION_FAILED_EXT;
			++streaming->manifested;
		}
		return SubmitToStream(user, queue, submit);
	}

	void RecordManifest(rhi::CommandList commands, const org::experimental::ExecutionBoundaryManifest& manifest)
	{
		std::vector<rhi::vulkan::ResourceAccessDeclaration> declarations;
		declarations.reserve(manifest.accesses.size());
		for (const auto& access : manifest.accesses) {
			rhi::vulkan::ResourceAccessDeclaration declaration;
			declaration.resource = access.backing;
			declaration.offset = access.offset;
			declaration.size = access.size;
			declaration.range = {access.subresources.mip, access.subresources.mips,
				access.subresources.slice, access.subresources.slices};
			declaration.aspects = access.aspects;
			declaration.sync = static_cast<rhi::ResourceSyncState>(access.state.sync);
			declaration.access = static_cast<rhi::ResourceAccessType>(access.state.access);
			declaration.layout = static_cast<rhi::ResourceLayout>(access.state.layout);
			declaration.write = access.state.write;
			declarations.push_back(declaration);
		}
		if (rhi::vulkan::set_command_list_resource_accesses(commands,
			{declarations.data(), static_cast<uint32_t>(declarations.size())}) != rhi::Result::Ok)
			throw std::runtime_error("Graph boundary manifest rejected by BasicRHI");
	}


	struct TestGateGuard {
		VkDevice device = VK_NULL_HANDLE;
		VkSemaphore gate = VK_NULL_HANDLE;
		~TestGateGuard() {
			// An assertion failure must not leave teardown waiting on our gate.
			if (gate) {
				VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
				signal.semaphore = gate;
				signal.value = 9;
				vkSignalSemaphore(device, &signal);
			}
		}
	};

	struct ScopedBarrier {
		PFN_vkCmdPipelineBarrier2 emit = nullptr;
		std::vector<VkBufferMemoryBarrier2> barriers;
		std::atomic<uint32_t> recorded = 0;
	};

	void RecordScopedBarrier(void* user, VkCommandBuffer commands)
	{
		auto& scoped = *static_cast<ScopedBarrier*>(user);
		VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(scoped.barriers.size());
		dependency.pBufferMemoryBarriers = scoped.barriers.data();
		scoped.emit(commands, &dependency);
		++scoped.recorded;
	}

	void ResolveScopedBarrier(ScopedBarrier& output, VkBuffer buffer,
		const dxvk::DxvkExternalAccessLedger::Transaction& transaction)
	{
		for (const auto& dependency : transaction.dependencies) {
			VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
			barrier.srcStageMask = dependency.srcStages;
			barrier.srcAccessMask = dependency.srcAccess;
			barrier.dstStageMask = dependency.dstStages;
			barrier.dstAccessMask = dependency.dstAccess;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer = buffer;
			barrier.offset = dependency.begin;
			barrier.size = dependency.end - dependency.begin;
			output.barriers.push_back(barrier);
		}
	}

	struct Teardown
	{
		std::unique_ptr<org::PersistentGraphHost>* host = nullptr;
		rhi::DevicePtr* device = nullptr;
		bool fired = false;
	};

	void OnTeardown(void* user, VkDevice)
	{
		// What RenderGraphRuntime::Shutdown does when DXVK destroys its device.
		auto* teardown = static_cast<Teardown*>(user);
		teardown->fired = true;
		teardown->host->reset();
		teardown->device->Reset();
	}

	template <class T>
	T Resolve(HMODULE module, const char* name)
	{
		return reinterpret_cast<T>(reinterpret_cast<void*>(::GetProcAddress(module, name)));
	}
}

int main(int argc, char** argv)
{
	REQUIRE(argc >= 2, "usage: OrgDxvkInteropTest <dxvk build dir> [lock|stream|resources|scoped|leased]");
	const std::filesystem::path dir = argv[1];
	const bool nativeImage = argc >= 3 && std::strcmp(argv[2], "native-image") == 0;
	const bool nativeBuffer = nativeImage || (argc >= 3 && std::strcmp(argv[2], "native-buffer") == 0);
	const bool leased = argc >= 3 && std::strcmp(argv[2], "leased") == 0;
	const bool scoped = argc >= 3 && std::strcmp(argv[2], "scoped") == 0;
	const bool resources = nativeBuffer || leased || scoped || (argc >= 3 && std::strcmp(argv[2], "resources") == 0);
	const bool stream = resources || (argc >= 3 && std::strcmp(argv[2], "stream") == 0);

	HMODULE dxgi = ::LoadLibraryExW((dir / L"dxvk_dxgi.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	HMODULE d3d11 = ::LoadLibraryExW((dir / L"dxvk_d3d11.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	REQUIRE(dxgi && d3d11, "load the DXVK DLLs");

	auto requestFeatures = Resolve<PFN_dxvkRequestDeviceFeatures>(d3d11, "dxvkRequestDeviceFeatures");
	auto getInfo = Resolve<PFN_dxvkGetInteropDeviceInfo>(d3d11, "dxvkGetInteropDeviceInfo");
	auto wrapBuffer = Resolve<PFN_dxvkCreateBufferFromVkBuffer>(d3d11, "dxvkCreateBufferFromVkBuffer");
	auto setTeardown = Resolve<PFN_dxvkSetDeviceTeardownCallback>(d3d11, "dxvkSetDeviceTeardownCallback");
	auto enqueue = Resolve<PFN_dxvkEnqueueInteropSubmission>(d3d11, "dxvkEnqueueInteropSubmission");
	auto emitCommands = Resolve<PFN_dxvkEmitCommandBufferCallback>(d3d11, "dxvkEmitCommandBufferCallback");
	auto registerResource = Resolve<PFN_dxvkRegisterInteropResource>(d3d11, "dxvkRegisterInteropResource");
	auto unregisterResource = Resolve<PFN_dxvkUnregisterInteropResource>(d3d11, "dxvkUnregisterInteropResource");
	auto enqueueBufferHandoff = Resolve<PFN_dxvkEnqueueBufferHandoff>(d3d11, "dxvkEnqueueBufferHandoff");
	auto getResourceInterface = Resolve<PFN_dxvkGetResourceInteropInterface>(d3d11, "dxvkGetResourceInteropInterface");
	auto createDevice = Resolve<decltype(&D3D11CreateDevice)>(d3d11, "D3D11CreateDevice");
	REQUIRE(requestFeatures && getInfo && wrapBuffer && setTeardown && enqueue && createDevice && emitCommands && registerResource && unregisterResource && getResourceInterface, "DXVK interop exports");

	const DxvkOrgInteropFeature features[] = {
		{ VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, "descriptorHeap" },
		{ nullptr, "vulkanMemoryModelDeviceScope" },
	};
	const DxvkOrgInteropFeatureRequest request{ DXVK_ORG_INTEROP_VERSION, static_cast<uint32_t>(std::size(features)), features };
	REQUIRE(SUCCEEDED(requestFeatures(&request)), "dxvkRequestDeviceFeatures");

	ComPtr<ID3D11Device> d3dDevice;
	ComPtr<ID3D11DeviceContext> context;
	const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
	REQUIRE(SUCCEEDED(createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &d3dDevice, nullptr, &context)),
		"D3D11CreateDevice through DXVK");

	ComPtr<IDXGIVkInteropDevice> interop;
	REQUIRE(SUCCEEDED(d3dDevice.As(&interop)), "IDXGIVkInteropDevice");
	DxvkOrgInteropDeviceInfo info{};
	info.version = DXVK_ORG_INTEROP_VERSION;
	REQUIRE(SUCCEEDED(getInfo(d3dDevice.Get(), &info)), "dxvkGetInteropDeviceInfo");
	bool hasHeap = false;
	for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
		hasHeap |= std::strcmp(info.enabledExtensions[i], VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME) == 0;
	if (!hasHeap) {
		std::puts("SKIP: the device does not support VK_EXT_descriptor_heap");
		return 0;
	}
	REQUIRE(info.grantedFeatureCount == std::size(features) && info.deniedFeatureCount == 0, "interop features granted");

	Locking locking{ interop.Get() };
	rhi::vulkan::AdoptedVulkanDeviceInfo adopt{};
	adopt.getInstanceProcAddr = info.getInstanceProcAddr;
	adopt.instance = info.instance;
	adopt.instanceApiVersion = info.instanceApiVersion;
	adopt.physicalDevice = info.physicalDevice;
	adopt.device = info.device;
	adopt.enabledDeviceExtensions = info.enabledExtensions;
	adopt.enabledDeviceExtensionCount = info.enabledExtensionCount;
	adopt.enabledFeatureChain = info.enabledFeatures;
	adopt.queues[0] = { info.graphicsQueue, info.graphicsQueueFamily, info.graphicsQueueIndex };
	Streaming streaming{ d3dDevice.Get(), enqueue };
	DxvkOrgInteropResourceInterface resourceInterface{DXVK_ORG_RESOURCE_INTERFACE_VERSION};
	REQUIRE(SUCCEEDED(getResourceInterface(d3dDevice.Get(), &resourceInterface)), "resource interop capability negotiation");
	REQUIRE((resourceInterface.capabilities & (DXVK_ORG_CAP_RESOURCE_REGISTRATION | DXVK_ORG_CAP_RETAINED_SUBMISSION)) ==
		(DXVK_ORG_CAP_RESOURCE_REGISTRATION | DXVK_ORG_CAP_RETAINED_SUBMISSION), "retained submission capability");
	REQUIRE(!(resourceInterface.capabilities & DXVK_ORG_CAP_SCOPED_SYNCHRONIZATION), "unfinished automatic synchronization is not advertised");
	if (leased || nativeBuffer) streaming.resources = resourceInterface;
	if (nativeBuffer) { REQUIRE(enqueueBufferHandoff, "native buffer handoff export"); streaming.bufferHandoff = enqueueBufferHandoff; }
	if (stream)
		adopt.submissionHooks = { &streaming, nullptr, nullptr, &SubmitToStream };
	else
		adopt.submissionHooks = { &locking, &LockQueue, &UnlockQueue };
	if (resources) adopt.submissionHooks.submitResources = &SubmitResourcesToStream;
	rhi::DevicePtr device;
	REQUIRE(rhi::vulkan::AdoptVulkanDevice(adopt, device) == rhi::Result::Ok, "AdoptVulkanDevice");
	if (nativeImage) {
		for (unsigned generation = 0; generation < 3; ++generation)
			REQUIRE(org::tests::ImageHandoff(d3dDevice.Get(), context.Get(), info, resourceInterface,
				Resolve<PFN_dxvkEnqueueResourceHandoff>(d3d11, "dxvkEnqueueResourceHandoff")) == 0, "native image handoff and replacement");
	}

	org::PersistentGraphHost::Desc hostDesc{
		.device = device.Get(), .backend = rhi::Backend::Vulkan, .queueBoundary = { .entry = true, .exit = true } };
	if (scoped || nativeBuffer) hostDesc.queueBoundary = {.entry = false, .exit = false};
	if (resources) hostDesc.boundaryManifestRecorder = &RecordManifest;
	std::unique_ptr<org::PersistentGraphHost> host = std::make_unique<org::PersistentGraphHost>(std::move(hostDesc));
	Teardown teardown{ &host, &device };
	REQUIRE(SUCCEEDED(setTeardown(&OnTeardown, &teardown)), "dxvkSetDeviceTeardownCallback");

	uint32_t value = 0;
	{
		auto program = LoadProgram(device.Get());
		REQUIRE(program, "compute program");
		auto values = org::Buffer::CreateUnmaterializedStructuredBuffer(kCount, sizeof(uint32_t), true);
		values->SetName("test.values");
		values->Materialize();
		host->AddExtension("test.write", [&] { return std::make_unique<WriteExtension>(values, program, &value); });

		// D3D11 sees the graph-owned buffer through a DXVK wrapper.
		rhi::VulkanResourceInfo resourceInfo{};
		REQUIRE(rhi::vulkan::get_resource_info(values->GetAPIResource(), resourceInfo), "graph buffer handle");
		D3D11_BUFFER_DESC wrapDesc{};
		wrapDesc.ByteWidth = kBytes;
		wrapDesc.Usage = D3D11_USAGE_DEFAULT;
		wrapDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		wrapDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		wrapDesc.StructureByteStride = sizeof(uint32_t);
		ComPtr<ID3D11Buffer> wrapped;
		REQUIRE(SUCCEEDED(wrapBuffer(d3dDevice.Get(), &wrapDesc, rhi::vulkan::from_native_void<VkBuffer>(resourceInfo.resource), &wrapped)),
			"dxvkCreateBufferFromVkBuffer");
		ComPtr<ID3D11ComputeShader> nativeShader;
		ComPtr<ID3D11ComputeShader> nativeInitializer;
		ComPtr<ID3D11UnorderedAccessView> nativeUav;
		ComPtr<ID3D11VertexShader> nativeVertexShader;
		ComPtr<ID3D11PixelShader> nativePixelShader;
		ComPtr<ID3D11ShaderResourceView> nativeSrv;
		ComPtr<ID3D11Texture2D> nativeTarget, nativeTargetReadback;
		ComPtr<ID3D11RenderTargetView> nativeRtv;
		if (nativeBuffer) {
			const char shader[] = "RWStructuredBuffer<uint> values : register(u0); "
				"[numthreads(64,1,1)] void main(uint3 id : SV_DispatchThreadID) { values[id.x] += 7; }";
			ComPtr<ID3DBlob> bytecode, errors;
			REQUIRE(SUCCEEDED(D3DCompile(shader, sizeof(shader) - 1, "NativeConsumer", nullptr, nullptr,
				"main", "cs_5_0", 0, 0, &bytecode, &errors)), "compile native buffer consumer");
			REQUIRE(SUCCEEDED(d3dDevice->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr,
				&nativeShader)), "create native buffer consumer");
			const char initializer[] = "RWStructuredBuffer<uint> values : register(u0); "
				"[numthreads(64,1,1)] void main(uint3 id : SV_DispatchThreadID) { values[id.x] = 0xDEADBEEF; }";
			REQUIRE(SUCCEEDED(D3DCompile(initializer, sizeof(initializer) - 1, "NativeProducer", nullptr, nullptr,
				"main", "cs_5_0", 0, 0, &bytecode, &errors)), "compile native buffer producer");
			REQUIRE(SUCCEEDED(d3dDevice->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr,
				&nativeInitializer)), "create native buffer producer");
			REQUIRE(SUCCEEDED(d3dDevice->CreateUnorderedAccessView(wrapped.Get(), nullptr, &nativeUav)), "create native UAV");
			const char vertex[] = "float4 main(uint id : SV_VertexID) : SV_Position { "
				"return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1); }";
			REQUIRE(SUCCEEDED(D3DCompile(vertex, sizeof(vertex) - 1, "NativeVertex", nullptr, nullptr,
				"main", "vs_5_0", 0, 0, &bytecode, &errors)), "compile native vertex shader");
			REQUIRE(SUCCEEDED(d3dDevice->CreateVertexShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr,
				&nativeVertexShader)), "create native vertex shader");
			const char pixel[] = "StructuredBuffer<uint> values : register(t0); "
				"uint main(float4 pos : SV_Position) : SV_Target { return values[uint(pos.x)]; }";
			REQUIRE(SUCCEEDED(D3DCompile(pixel, sizeof(pixel) - 1, "NativePixel", nullptr, nullptr,
				"main", "ps_5_0", 0, 0, &bytecode, &errors)), "compile native pixel shader");
			REQUIRE(SUCCEEDED(d3dDevice->CreatePixelShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr,
				&nativePixelShader)), "create native pixel shader");
			REQUIRE(SUCCEEDED(d3dDevice->CreateShaderResourceView(wrapped.Get(), nullptr, &nativeSrv)), "create native SRV");
			D3D11_TEXTURE2D_DESC targetDesc{};
			targetDesc.Width = kCount;
			targetDesc.Height = targetDesc.MipLevels = targetDesc.ArraySize = targetDesc.SampleDesc.Count = 1;
			targetDesc.Format = DXGI_FORMAT_R32_UINT;
			targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
			REQUIRE(SUCCEEDED(d3dDevice->CreateTexture2D(&targetDesc, nullptr, &nativeTarget)), "create graphics readback target");
			REQUIRE(SUCCEEDED(d3dDevice->CreateRenderTargetView(nativeTarget.Get(), nullptr, &nativeRtv)), "create graphics RTV");
			targetDesc.BindFlags = 0;
			targetDesc.Usage = D3D11_USAGE_STAGING;
			targetDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			REQUIRE(SUCCEEDED(d3dDevice->CreateTexture2D(&targetDesc, nullptr, &nativeTargetReadback)), "create graphics staging target");
		}
		DxvkOrgInteropRegistration first{DXVK_ORG_RESOURCE_REGISTRATION_VERSION};
		DxvkOrgInteropRegistration second{DXVK_ORG_RESOURCE_REGISTRATION_VERSION};
		REQUIRE(SUCCEEDED(registerResource(d3dDevice.Get(), wrapped.Get(), &first)), "register native buffer");
		REQUIRE(SUCCEEDED(registerResource(d3dDevice.Get(), wrapped.Get(), &second)), "register second native buffer lease");
		REQUIRE(first.leaseToken && second.leaseToken && first.leaseToken != second.leaseToken, "independent registration leases");
		REQUIRE(first.backingToken && first.backingToken == second.backingToken, "canonical native backing identity");
		REQUIRE(first.resource.buffer.buffer == rhi::vulkan::from_native_void<VkBuffer>(resourceInfo.resource)
			&& first.resource.buffer.size == kBytes && first.queueFamily == info.graphicsQueueFamily,
			"registered native extent and queue");
		REQUIRE(first.legalStages && first.legalAccess, "registration carries conservative bootstrap scope");
		REQUIRE(SUCCEEDED(unregisterResource(d3dDevice.Get(), first.leaseToken)), "retire first lease");
		REQUIRE(FAILED(unregisterResource(d3dDevice.Get(), first.leaseToken)), "reject duplicate retirement");
		DxvkOrgInteropRegistration third{DXVK_ORG_RESOURCE_REGISTRATION_VERSION};
		REQUIRE(SUCCEEDED(registerResource(d3dDevice.Get(), wrapped.Get(), &third)), "register while another lease is live");
		REQUIRE(third.backingToken == second.backingToken, "live backing generation survives partial retirement");
		REQUIRE(SUCCEEDED(unregisterResource(d3dDevice.Get(), second.leaseToken)), "retire second lease");
		REQUIRE(SUCCEEDED(unregisterResource(d3dDevice.Get(), third.leaseToken)), "retire last lease");
		third = {DXVK_ORG_RESOURCE_REGISTRATION_VERSION};
		REQUIRE(SUCCEEDED(registerResource(d3dDevice.Get(), wrapped.Get(), &third)), "register retired handle again");
		REQUIRE(third.backingToken != first.backingToken, "retired handles cannot inherit old ledger state");
		REQUIRE(SUCCEEDED(unregisterResource(d3dDevice.Get(), third.leaseToken)), "retire replacement registration");

		D3D11_TEXTURE2D_DESC textureDesc{};
		textureDesc.Width = textureDesc.Height = 16;
		textureDesc.MipLevels = 3;
		textureDesc.ArraySize = 2;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.Usage = D3D11_USAGE_DEFAULT;
		textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ComPtr<ID3D11Texture2D> texture;
		REQUIRE(SUCCEEDED(d3dDevice->CreateTexture2D(&textureDesc, nullptr, &texture)), "registration test image");
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		viewDesc.Format = textureDesc.Format;
		viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		viewDesc.Texture2DArray.MostDetailedMip = 1;
		viewDesc.Texture2DArray.MipLevels = 1;
		viewDesc.Texture2DArray.FirstArraySlice = 1;
		viewDesc.Texture2DArray.ArraySize = 1;
		ComPtr<ID3D11ShaderResourceView> view;
		REQUIRE(SUCCEEDED(d3dDevice->CreateShaderResourceView(texture.Get(), &viewDesc, &view)), "registration test image view");
		first = second = {DXVK_ORG_RESOURCE_REGISTRATION_VERSION};
		REQUIRE(SUCCEEDED(registerResource(d3dDevice.Get(), texture.Get(), &first)), "register whole image");
		REQUIRE(SUCCEEDED(registerResource(d3dDevice.Get(), view.Get(), &second)), "register image view");
		REQUIRE(first.backingToken == second.backingToken, "views share image backing identity");
		const auto& viewRange = second.resource.image.subresourceRange;
		REQUIRE(viewRange.baseMipLevel == 1 && viewRange.levelCount == 1 && viewRange.baseArrayLayer == 1
			&& viewRange.layerCount == 1 && viewRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT, "view retains exact subresources");
		view.Reset();
		texture.Reset();
		REQUIRE(SUCCEEDED(unregisterResource(d3dDevice.Get(), first.leaseToken)), "lease survives D3D11 resource destruction");
		REQUIRE(SUCCEEDED(unregisterResource(d3dDevice.Get(), second.leaseToken)), "retire native image after its last view");
		D3D11_BUFFER_DESC dynamicDesc = wrapDesc;
		dynamicDesc.Usage = D3D11_USAGE_DYNAMIC;
		dynamicDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		ComPtr<ID3D11Buffer> rejected;
		REQUIRE(FAILED(wrapBuffer(d3dDevice.Get(), &dynamicDesc, rhi::vulkan::from_native_void<VkBuffer>(resourceInfo.resource), &rejected)),
			"DXVK refuses to wrap an external buffer it would have to rename");

		ComPtr<ID3D11Buffer> dynamicBuffer;
		REQUIRE(SUCCEEDED(d3dDevice->CreateBuffer(&dynamicDesc, nullptr, &dynamicBuffer)), "native dynamic buffer");
		DxvkOrgInteropRegistration invalid{DXVK_ORG_RESOURCE_REGISTRATION_VERSION};
		REQUIRE(FAILED(registerResource(d3dDevice.Get(), dynamicBuffer.Get(), &invalid)), "reject renamable buffer registration");
		REQUIRE(!invalid.leaseToken && !invalid.backingToken, "failed registration publishes no tokens");
		invalid.version = DXVK_ORG_RESOURCE_REGISTRATION_VERSION + 1;
		REQUIRE(FAILED(registerResource(d3dDevice.Get(), wrapped.Get(), &invalid)), "reject incompatible registration version");
		REQUIRE(FAILED(unregisterResource(d3dDevice.Get(), 0)), "reject null registration retirement");

		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = kBytes;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Buffer> staging;
		REQUIRE(SUCCEEDED(d3dDevice->CreateBuffer(&stagingDesc, nullptr, &staging)), "staging buffer");

		const auto nativeBufferHandle = rhi::vulkan::from_native_void<VkBuffer>(resourceInfo.resource);
		const auto getDeviceProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(info.getInstanceProcAddr(info.instance, "vkGetDeviceProcAddr"));
		const auto barrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(getDeviceProc(info.device, "vkCmdPipelineBarrier2"));
		REQUIRE(barrier2, "synchronization2 barrier entry point");
		VkSemaphore timelineGate = VK_NULL_HANDLE;
		TestGateGuard gateGuard{info.device};
		if (leased) {
			VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
			type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
			VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
			REQUIRE(vkCreateSemaphore(info.device, &create, nullptr, &timelineGate) == VK_SUCCESS, "GPU completion test gate");
			streaming.gate = timelineGate;
			gateGuard.gate = timelineGate;
		}
		DxvkOrgInteropRegistration persistentLease{DXVK_ORG_RESOURCE_REGISTRATION_VERSION};
		if (nativeBuffer) {
			REQUIRE(SUCCEEDED(resourceInterface.registerResource(resourceInterface.context, wrapped.Get(), &persistentLease)), "register native tracked buffer");
			streaming.lease = persistentLease.leaseToken;
			streaming.registeredBuffer = persistentLease.resource.buffer.buffer;
		}
		const std::vector<uint32_t> sentinel(kCount, nativeBuffer ? 0xDEADBE00u : 0xDEADBEEFu);
		for (uint32_t frame = 0; frame < 8; ++frame) {
			value = 5000u + frame * 100u;
			DxvkOrgInteropRegistration gpuLease{DXVK_ORG_RESOURCE_REGISTRATION_VERSION};
			if (leased) {
				REQUIRE(SUCCEEDED(resourceInterface.registerResource(resourceInterface.context, wrapped.Get(), &gpuLease)), "register submission lease");
				streaming.lease = gpuLease.leaseToken;
				streaming.gateValue = frame + 1;
				VkSubmitInfo2 emptySubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
				const uint64_t invalidTokens[] = {gpuLease.leaseToken, 0};
				DxvkOrgInteropLeasedSubmission rejectedSubmit{};
				rejectedSubmit.version = DXVK_ORG_RESOURCE_INTERFACE_VERSION;
				rejectedSubmit.batch.version = DXVK_ORG_INTEROP_VERSION;
				rejectedSubmit.batch.submitCount = 1;
				rejectedSubmit.batch.submits = &emptySubmit;
				rejectedSubmit.batch.onSubmitted = &OnStreamSubmitted;
				rejectedSubmit.batch.user = &streaming;
				rejectedSubmit.leaseCount = 2;
				rejectedSubmit.leaseTokens = invalidTokens;
				rejectedSubmit.onCompleted = &OnStreamCompleted;
				rejectedSubmit.completionUser = &streaming;
				REQUIRE(FAILED(resourceInterface.enqueue(resourceInterface.context, &rejectedSubmit)), "mixed valid and invalid leases are rejected transactionally");
				rejectedSubmit.leaseCount = 1;
				emptySubmit.flags = VK_SUBMIT_PROTECTED_BIT;
				REQUIRE(FAILED(resourceInterface.enqueue(resourceInterface.context, &rejectedSubmit)), "unsupported submit flags are rejected before acceptance");
			}
			// D3D11 writes the buffer before the epoch: the graph must run after this.
			context->UpdateSubresource(wrapped.Get(), 0, nullptr, sentinel.data(), 0, 0);
			if (nativeBuffer) {
				context->CSSetShader(nativeInitializer.Get(), nullptr, 0);
				ID3D11UnorderedAccessView* view = nativeUav.Get();
				context->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
				context->Dispatch(kCount / 64, 1, 1);
				view = nullptr;
				context->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
			}
			// This controlled test declares its native accesses explicitly. Production
			// DXVK producer/consumer discovery is a separate integration requirement.
			dxvk::DxvkExternalAccessLedger ledger;
			ScopedBarrier incoming, outgoing;
			incoming.emit = outgoing.emit = barrier2;
			if (scoped) {
				ledger.registerResource(1);
				auto producer = ledger.prepare({{1, 0, 0, kBytes, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true}});
				ledger.commit(std::move(producer));
				auto epoch = ledger.prepare({{1, 0, 0, kBytes, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, true}});
				ResolveScopedBarrier(incoming, nativeBufferHandle, epoch);
				REQUIRE(incoming.barriers.size() == 1, "one scoped native-to-ORG dependency");
				REQUIRE(SUCCEEDED(emitCommands(d3dDevice.Get(), &RecordScopedBarrier, &incoming)), "enqueue incoming dependency");
				ledger.commit(std::move(epoch));
			}
			// RenderGraphRuntime::ExecuteEpoch.
			if (!stream)
				interop->FlushRenderingCommands();
			host->ExecuteFrame();
			if (leased) {
				REQUIRE(streaming.completed == static_cast<int>(frame), "acceptance does not signal GPU completion");
				REQUIRE(SUCCEEDED(resourceInterface.unregisterResource(resourceInterface.context, gpuLease.leaseToken)), "release client lease before GPU completion");
				DxvkOrgInteropRegistration probe{DXVK_ORG_RESOURCE_REGISTRATION_VERSION};
				REQUIRE(SUCCEEDED(resourceInterface.registerResource(resourceInterface.context, wrapped.Get(), &probe)), "probe in-flight native identity");
				REQUIRE(probe.backingToken == gpuLease.backingToken, "accepted submission retains backing generation");
				REQUIRE(SUCCEEDED(resourceInterface.unregisterResource(resourceInterface.context, probe.leaseToken)), "release probe lease");
				VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
				signal.semaphore = timelineGate;
				signal.value = frame + 1;
				REQUIRE(vkSignalSemaphore(info.device, &signal) == VK_SUCCESS, "release GPU completion gate");
			}
			if (scoped) {
				auto consumer = ledger.prepare({{1, 0, 0, kBytes, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false}});
				ResolveScopedBarrier(outgoing, nativeBufferHandle, consumer);
				REQUIRE(outgoing.barriers.size() == 1, "one scoped ORG-to-native dependency");
				REQUIRE(SUCCEEDED(emitCommands(d3dDevice.Get(), &RecordScopedBarrier, &outgoing)), "enqueue consumer dependency");
				ledger.commit(std::move(consumer));
			}
			if (nativeBuffer) {
				// Ordinary read-only graphics skips DXVK's storage-hazard checks.
				// Read the ORG output directly before any native compute consumer.
				context->VSSetShader(nativeVertexShader.Get(), nullptr, 0);
				context->PSSetShader(nativePixelShader.Get(), nullptr, 0);
				context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				D3D11_VIEWPORT viewport{0, 0, float(kCount), 1, 0, 1};
				context->RSSetViewports(1, &viewport);
				ID3D11RenderTargetView* target = nativeRtv.Get();
				ID3D11ShaderResourceView* resource = nativeSrv.Get();
				context->OMSetRenderTargets(1, &target, nullptr);
				context->PSSetShaderResources(0, 1, &resource);
				context->Draw(3, 0);
				context->Draw(3, 0);
				resource = nullptr;
				context->PSSetShaderResources(0, 1, &resource);
				context->OMSetRenderTargets(0, nullptr, nullptr);
				context->CopyResource(nativeTargetReadback.Get(), nativeTarget.Get());
				D3D11_MAPPED_SUBRESOURCE pixels{};
				REQUIRE(SUCCEEDED(context->Map(nativeTargetReadback.Get(), 0, D3D11_MAP_READ, 0, &pixels)), "map native graphics consumer");
				bool pixelsMatch = true;
				for (uint32_t i = 0; i < kCount; ++i)
					pixelsMatch &= static_cast<const uint32_t*>(pixels.pData)[i] == value + i;
				context->Unmap(nativeTargetReadback.Get(), 0);
				REQUIRE(pixelsMatch, "read-only native graphics observes ORG writes");
				context->CSSetShader(nativeShader.Get(), nullptr, 0);
				ID3D11UnorderedAccessView* view = nativeUav.Get();
				context->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
				context->Dispatch(kCount / 64, 1, 1);
				// A second dispatch keeps descriptors clean and checks retained native
				// write provenance, not just first-use resource acquisition.
				context->Dispatch(kCount / 64, 1, 1);
				view = nullptr;
				context->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
			}
			// Recorded after the epoch on the same queue: must observe this frame's values.
			context->CopyResource(staging.Get(), wrapped.Get());
			D3D11_MAPPED_SUBRESOURCE mapped{};
			REQUIRE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map staging");
			const auto* words = static_cast<const uint32_t*>(mapped.pData);
			bool match = true;
			for (uint32_t i = 0; i < kCount && match; ++i)
				match = words[i] == value + i + (nativeBuffer ? 14u : 0u);
			const uint32_t first = words[0];
			context->Unmap(staging.Get(), 0);
			if (scoped) REQUIRE(incoming.recorded == 1 && outgoing.recorded == 1, "both dependencies recorded in stream order");
			if (leased) {
				for (uint32_t tries = 0; streaming.completed != static_cast<int>(frame + 1) && tries < 2000; ++tries)
					Sleep(1);
				REQUIRE(streaming.completed == static_cast<int>(frame + 1), "completion callback follows GPU retirement");
				DxvkOrgInteropRegistration probe{DXVK_ORG_RESOURCE_REGISTRATION_VERSION};
				REQUIRE(SUCCEEDED(resourceInterface.registerResource(resourceInterface.context, wrapped.Get(), &probe)), "probe retired identity");
				REQUIRE(probe.backingToken != gpuLease.backingToken, "GPU completion releases the last backing lease");
				REQUIRE(SUCCEEDED(resourceInterface.unregisterResource(resourceInterface.context, probe.leaseToken)), "release retired probe");
			}
			if (!match) {
				std::fprintf(stderr, "FAIL: frame %u read %u, expected %u\n", frame, first, value + (nativeBuffer ? 14u : 0u));
				return 1;
			}
		}
		if (nativeBuffer) REQUIRE(SUCCEEDED(resourceInterface.unregisterResource(resourceInterface.context, persistentLease.leaseToken)), "retire native tracked buffer");
		if (leased) { vkDestroySemaphore(info.device, timelineGate, nullptr); gateGuard.gate = VK_NULL_HANDLE; }
		if (resources) REQUIRE(streaming.manifested == 8, "each graph execution delivered its concrete manifest");
		if (stream) {
			REQUIRE(streaming.enqueued >= 8 && streaming.failed == 0, "graph submissions through DXVK's stream");
			REQUIRE(locking.locks == 0, "no locked submissions in stream mode");
		} else {
			REQUIRE(locking.locks >= 8 && !locking.unbalanced && locking.depth == 0, "graph submissions under DXVK's lock");
		}
	}

	// DXVK tears its device down with the last D3D11 reference; the callback retires ORG first.
	context.Reset();
	interop.Reset();
	d3dDevice.Reset();
	REQUIRE(teardown.fired, "DXVK teardown callback fired");
	REQUIRE(!host && !device, "render graph retired before the VkDevice");

	if (stream)
		REQUIRE(streaming.submitted == streaming.enqueued, "every streamed submission reached the queue");
	std::printf("OrgDxvkInteropTest: ok (%s: %d locked, %d streamed submissions)\n", stream ? "stream" : "lock", locking.locks,
		streaming.submitted.load());
	return 0;
}

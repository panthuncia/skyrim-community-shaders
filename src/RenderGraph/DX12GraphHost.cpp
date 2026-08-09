#include "DX12GraphHost.h"

#include <Render/RenderGraph/RenderGraph.h>
#include <Render/Runtime/RuntimeDevice.h>
#include <Render/Runtime/OpenRenderGraphSettings.h>
#include <Render/MemoryIntrospectionBackend.h>
#include <RenderPasses/Base/ComputePass.h>
#include <Interfaces/IDynamicDeclaredResources.h>
#include <Resources/ExternalBackingResource.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/GPUBacking/GpuBufferBacking.h>
#include <Resources/PixelBuffer.h>
#include <rhi_helpers.h>
#include <rhi_interop_dx12.h>
#if defined(CS_HAS_ORG_MODULE_SERVICES)
#include <ORGModuleServices/FrameUploadArena.h>
#include <ORGModuleServices/ShaderCompiler.h>
#include <ORGModuleServices/PipelineService.h>
#endif
#include <deque>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace
{
	struct ExecutionHost
	{
#if defined(CS_HAS_ORG_MODULE_SERVICES)
		org::services::FrameUploadArena* uploads{};
#endif
		std::unordered_map<CSDX12ResourceHandle, std::shared_ptr<org::Resource>> resources;
		std::unordered_set<CSDX12ResourceHandle> d3d11ProducedImports;
	};
	constexpr org::ExternalTimelineBinding kD3D11ReadyBinding = 1;

	struct EpochState
	{
		rhi::Timeline readyTimeline{};
		uint64_t readyValue{};
		rhi::Timeline completeTimeline{};
		uint64_t completeValue{};
		CSDX12FrameInfo frame{};
		std::vector<DX12GraphHost::WorkItem> workItems;
		ExecutionHost* host{};
	};

	CSDX12Status CS_DX12_GRAPH_CALL ResourceLookup(const CSDX12ExecutionContext* context, CSDX12ResourceHandle handle, void** out)
	{
		if (!context || !context->hostContext || !out) return CS_DX12_E_INVALID_ARGUMENT;
		*out = nullptr;
		auto* host = static_cast<ExecutionHost*>(context->hostContext);
		const auto it = host->resources.find(handle);
		if (it == host->resources.end() || !it->second) return CS_DX12_E_STALE_HANDLE;
		*out = rhi::dx12::get_resource(it->second->GetAPIResource());
		return *out ? CS_DX12_OK : CS_DX12_E_NOT_READY;
	}

	CSDX12Status CS_DX12_GRAPH_CALL AllocateUpload(const CSDX12ExecutionContext* context, uint64_t size, uint64_t alignment, CSDX12UploadAllocation* out)
	{
#if defined(CS_HAS_ORG_MODULE_SERVICES)
		if (!context || !context->frame || !context->hostContext || !out || !size) return CS_DX12_E_INVALID_ARGUMENT;
		try {
			auto* host = static_cast<ExecutionHost*>(context->hostContext);
			if (!host->uploads) return CS_DX12_E_UNSUPPORTED_CAPABILITY;
			auto allocation = host->uploads->Allocate(
				size, alignment, context->frame->completionValue);
			*out = { allocation.cpuAddress, allocation.gpuAddress, allocation.size, allocation.resource };
			return CS_DX12_OK;
		} catch (...) { return CS_DX12_E_INTERNAL; }
#else
		(void)context; (void)size; (void)alignment; (void)out;
		return CS_DX12_E_UNSUPPORTED_CAPABILITY;
#endif
	}

	CSDX12Status CS_DX12_GRAPH_CALL AllocateDescriptors(const CSDX12ExecutionContext* context, uint32_t heapType, uint32_t count, CSDX12DescriptorAllocation* out)
	{
#if defined(CS_HAS_ORG_MODULE_SERVICES)
		if (!context || !context->frame || !context->hostContext || !out) return CS_DX12_E_INVALID_ARGUMENT;
		try {
			auto* host = static_cast<ExecutionHost*>(context->hostContext);
			if (!host->uploads) return CS_DX12_E_UNSUPPORTED_CAPABILITY;
			auto allocation = host->uploads->AllocateDescriptors(
				heapType, count, context->frame->completionValue);
			*out = { allocation.cpuHandle, allocation.gpuHandle, allocation.descriptorSize, count, allocation.heap };
			return CS_DX12_OK;
		} catch (...) { return CS_DX12_E_INTERNAL; }
#else
		(void)context; (void)heapType; (void)count; (void)out;
		return CS_DX12_E_UNSUPPORTED_CAPABILITY;
#endif
	}

	class EpochPass final : public org::ComputePass, public org::IDynamicDeclaredResources
	{
	public:
		EpochPass(std::shared_ptr<EpochState> state, size_t workIndex) : state(std::move(state)), workIndex(workIndex) {}
		void Setup() override {}
		void Cleanup() override {}
		bool DeclaredResourcesChanged() const override { return true; }
		bool RequiresPassRebindAfterDeclarationRefresh() const noexcept override { return false; }
		void DeclareResourceUsages(org::ComputePassBuilder* builder) override
		{
			// D3D11 produces imported textures immediately before the epoch and
			// signals readyTimeline only after those writes.  Put the external wait
			// on the queue that will perform this pass's transitions and execution;
			// relying on a resource-less graphics boundary to propagate the wait to
			// compute/copy queues can allow an imported resource to be transitioned
			// or sampled before the D3D11 copy has completed.
			bool consumesD3D11Import = false;
			for (const auto& access : state->workItems[workIndex].accesses) {
				const auto it = state->host->resources.find(access.resource);
				if (it == state->host->resources.end()) throw std::runtime_error("Missing ORG resource for declared pass access");
				const bool readsResource = (access.access & (CS_DX12_ACCESS_SHADER_READ |
					CS_DX12_ACCESS_CONSTANT_BUFFER | CS_DX12_ACCESS_DEPTH_READ |
					CS_DX12_ACCESS_COPY_SOURCE | CS_DX12_ACCESS_INDIRECT_ARGUMENT)) != 0;
				consumesD3D11Import |= readsResource && state->host->d3d11ProducedImports.contains(access.resource);
				const auto& resource = it->second;
				if (access.access & CS_DX12_ACCESS_UNORDERED_WRITE) builder->WithUnorderedAccess(resource);
				else if (access.access & (CS_DX12_ACCESS_COPY_DESTINATION | CS_DX12_ACCESS_COPY_SOURCE)) builder->WithLegacyInterop(resource);
				else if (access.access & CS_DX12_ACCESS_CONSTANT_BUFFER) builder->WithConstantBuffer(resource);
				else builder->WithShaderResource(resource);
			}
			if (consumesD3D11Import) builder->WithExternalWaitBindingBeforeTransitions(kD3D11ReadyBinding);
		}
		org::PassReturn Execute(org::PassExecutionContext& context) override
		{
			auto* native = rhi::dx12::get_cmd_list(context.commandList);
			if (!native)
				throw std::runtime_error("ORG did not provide a native D3D12 command list");
			CSDX12ExecutionContext execution{};
			execution.structSize = sizeof(execution);
			execution.apiVersion = CS_DX12_GRAPH_API_CURRENT;
			execution.frame = &state->frame;
			execution.borrowedD3D12GraphicsCommandList = native;
			execution.hostContext = state->host;
			execution.GetResource = &ResourceLookup;
			execution.AllocateUpload = &AllocateUpload;
			execution.AllocateDescriptors = &AllocateDescriptors;
			const auto status = state->workItems[workIndex].execute(execution);
			if (status != CS_DX12_OK)
				throw std::runtime_error("DX12 contributor execution failed with status " + std::to_string(status));
			return {};
		}
	private:
		std::shared_ptr<EpochState> state;
		size_t workIndex{};
	};

	class AnchorPass final : public org::ComputePass
	{
	public:
		void Setup() override {}
		void Cleanup() override {}
		void DeclareResourceUsages(org::ComputePassBuilder*) override {}
		org::PassReturn Execute(org::PassExecutionContext&) override { return {}; }
	};

	class BoundaryPass final : public org::ComputePass, public org::IDynamicDeclaredResources
	{
	public:
		BoundaryPass(std::shared_ptr<EpochState> state, bool begin) : state(std::move(state)), begin(begin) {}
		void Setup() override {}
		void Cleanup() override {}
		bool DeclaredResourcesChanged() const override { return true; }
		bool RequiresPassRebindAfterDeclarationRefresh() const noexcept override { return false; }
		void DeclareResourceUsages(org::ComputePassBuilder* builder) override
		{
			if (!begin) for (const auto& [_, resource] : state->host->resources) builder->WithLegacyInterop(resource);
		}
		org::PassReturn Execute(org::PassExecutionContext&) override
		{
			org::PassReturn result{};
			if (!begin) result.externalSignalsAfterCompletion.push_back({ state->completeTimeline, state->completeValue });
			return result;
		}
	private:
		std::shared_ptr<EpochState> state;
		bool begin{};
	};

	class EpochExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		EpochExtension() : state(std::make_shared<EpochState>()) {}
		void SetFrame(EpochState value) {
			state->readyTimeline = value.readyTimeline; state->readyValue = value.readyValue;
			state->completeTimeline = value.completeTimeline; state->completeValue = value.completeValue;
			state->frame = value.frame;
			state->host = value.host;
		}
		void SetWorkItems(std::vector<DX12GraphHost::WorkItem> value) { state->workItems = std::move(value); }
		std::vector<DX12GraphHost::WorkItem> GetWorkItems() const { return state->workItems; }
		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& out) override {
			auto begin = org::RenderGraph::ExternalPassDesc::Compute("cs.dx12.epoch.begin", std::make_shared<BoundaryPass>(state, true))
				.PreferQueue(org::QueueKind::Graphics).CollectStatistics(false);
			auto end = org::RenderGraph::ExternalPassDesc::Compute("cs.dx12.epoch.end", std::make_shared<BoundaryPass>(state, false))
				.PreferQueue(org::QueueKind::Graphics).CollectStatistics(false);
			out.push_back(std::move(begin));
			constexpr const char* anchors[]{ CS_DX12_ANCHOR_FRAME_BEGIN, CS_DX12_ANCHOR_SHADOWS_READY,
				CS_DX12_ANCHOR_GBUFFER_READY, CS_DX12_ANCHOR_DEFERRED_LIGHTING_BEGIN,
				CS_DX12_ANCHOR_DEFERRED_LIGHTING_END, CS_DX12_ANCHOR_FRAME_END };
			const char* previous = "cs.dx12.epoch.begin";
			for (const auto* anchor : anchors) {
				auto anchorDesc = org::RenderGraph::ExternalPassDesc::Compute(anchor, std::make_shared<AnchorPass>())
					.At(org::RenderGraph::ExternalInsertPoint::After(previous)).PreferQueue(org::QueueKind::Graphics).CollectStatistics(false);
				out.push_back(std::move(anchorDesc));
				previous = anchor;
			}
			std::string previousSerialized;
			for (size_t i = 0; i < state->workItems.size(); ++i) {
				const auto& item = state->workItems[i];
				auto point = org::RenderGraph::ExternalInsertPoint::After("cs.dx12.epoch.begin");
				point.keepExtensionOrder = false;
				for (const auto& dependency : item.after) point.AlsoAfter(dependency);
				for (const auto& dependency : item.before) point.AlsoBefore(dependency);
				if ((item.flags & CS_DX12_PASS_PARALLEL_RECORDING_SAFE) == 0 && !previousSerialized.empty())
					point.AlsoAfter(previousSerialized);
				auto desc = org::RenderGraph::ExternalPassDesc::Compute(
					item.name, std::make_shared<EpochPass>(state, i)).At(std::move(point));
				switch (item.queuePolicy) {
				case CS_DX12_QUEUE_AUTOMATIC: desc.AutomaticQueueAssignment(); break;
				case CS_DX12_QUEUE_PREFER_COMPUTE:
				case CS_DX12_QUEUE_REQUIRE_COMPUTE: desc.PreferQueue(org::QueueKind::Compute); break;
				case CS_DX12_QUEUE_PREFER_COPY:
				case CS_DX12_QUEUE_REQUIRE_COPY: desc.PreferQueue(org::QueueKind::Copy); break;
				default: desc.PreferQueue(org::QueueKind::Graphics); break;
				}
				desc.collectStatistics = (item.flags & CS_DX12_PASS_DISABLE_STATISTICS) == 0;
				out.push_back(std::move(desc));
				if ((item.flags & CS_DX12_PASS_PARALLEL_RECORDING_SAFE) == 0) previousSerialized = item.name;
			}
			auto endPoint = org::RenderGraph::ExternalInsertPoint::After(CS_DX12_ANCHOR_FRAME_END);
			endPoint.keepExtensionOrder = false;
			for (const auto& item : state->workItems) endPoint.AlsoAfter(item.name);
			end.At(std::move(endPoint));
			out.push_back(std::move(end));
		}
	private:
		std::shared_ptr<EpochState> state;
	};
}

class DX12GraphHost::Impl
{
public:
	struct GraphGeneration
	{
		std::unique_ptr<org::RenderGraph> graph;
		EpochExtension* epochExtension{};
		ExecutionHost host{};
		uint64_t lastCompletion{};
		~GraphGeneration() { if (graph) graph->ShutdownExtensions(); }
	};

	explicit Impl(rhi::Device runtimeDevice) : device(runtimeDevice)
	{
		auto settings = org::runtime::GetOpenRenderGraphSettings();
		settings.collectPassStatistics = true;
		settings.collectPipelineStatistics = false;
		settings.renderGraphBatchTraceEnabled = false;
		org::runtime::SetOpenRenderGraphSettings(settings);
		org::runtime::InitializeRuntimeDevice(device);
#if defined(CS_HAS_ORG_MODULE_SERVICES)
		uploads = std::make_unique<org::services::FrameUploadArena>(rhi::dx12::get_device(device));
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
		shaderCompiler = std::make_unique<org::services::ShaderCompiler>(std::filesystem::path("Data/SKSE/Plugins/CommunityShaders/ShaderCache/ORG"));
#endif
		pipelines = std::make_unique<org::services::PipelineService>();
#endif
		active = Compile({}, {});
	}

	~Impl()
	{
		active.reset();
		retired.clear();
		org::runtime::ShutdownRuntimeDevice();
	}

	std::unique_ptr<GraphGeneration> Compile(
		std::vector<DX12GraphHost::ResourceDefinition> resources,
		std::vector<DX12GraphHost::WorkItem> workItems)
	{
		auto generation = std::make_unique<GraphGeneration>();
		generation->graph = std::make_unique<org::RenderGraph>(device);
#if defined(CS_HAS_ORG_MODULE_SERVICES)
		generation->host.uploads = uploads.get();
#endif
		for (const auto& definition : resources) {
			std::shared_ptr<org::Resource> resource;
			if (definition.desc.dimension == CS_DX12_RESOURCE_BUFFER) {
				if (definition.desc.byteSize == 0)
					throw std::runtime_error("CS ORG buffer resource has zero size");
				const bool unordered = (definition.desc.allowedAccess & CS_DX12_ACCESS_UNORDERED_WRITE) != 0;
				auto backing = org::GpuBufferBacking::CreateUnique(
					rhi::HeapType::DeviceLocal, definition.desc.byteSize, definition.handle,
					unordered, definition.name.c_str());
				resource = org::ExternalBackingResource::CreateShared(std::move(backing));
			} else if (definition.desc.dimension == CS_DX12_RESOURCE_TEXTURE_2D) {
				if (!definition.desc.width || !definition.desc.height || definition.desc.format == DXGI_FORMAT_UNKNOWN)
					throw std::runtime_error("CS ORG Texture2D resource has an invalid description");
				org::TextureDescription texture{};
				texture.imageDimensions.resize((std::max)(1u, definition.desc.mipLevels));
				uint32_t mipWidth = definition.desc.width;
				uint32_t mipHeight = definition.desc.height;
				for (auto& dimensions : texture.imageDimensions) {
					dimensions.width = mipWidth;
					dimensions.height = mipHeight;
					mipWidth = (std::max)(1u, mipWidth >> 1);
					mipHeight = (std::max)(1u, mipHeight >> 1);
				}
				texture.channels = 4;
				texture.format = rhi::helpers::ToRHI(static_cast<DXGI_FORMAT>(definition.desc.format));
				texture.arraySize = (std::max)(1u, definition.desc.depthOrArraySize);
				texture.isArray = texture.arraySize > 1;
				texture.hasSRV = (definition.desc.allowedAccess & CS_DX12_ACCESS_SHADER_READ) != 0;
				texture.hasUAV = (definition.desc.allowedAccess & CS_DX12_ACCESS_UNORDERED_WRITE) != 0;
				texture.hasRTV = (definition.desc.allowedAccess & CS_DX12_ACCESS_RENDER_TARGET) != 0;
				texture.initialLayout = definition.desc.lifetime == CS_DX12_RESOURCE_PERSISTENT ?
					rhi::ResourceLayout::Undefined : rhi::ResourceLayout::Common;

				if (definition.desc.lifetime == CS_DX12_RESOURCE_CS_IMPORTED ||
					definition.desc.lifetime == CS_DX12_RESOURCE_CONTRIBUTOR_IMPORTED) {
					auto* native = static_cast<ID3D12Resource*>(definition.desc.borrowedNativeResource);
					rhi::ResourcePtr imported;
					if (!native || rhi::Failed(rhi::dx12::import_resource(device, native, imported)))
						throw std::runtime_error("Failed to import native D3D12 texture into BasicRHI");
					resource = org::ExternalTextureResource::CreateShared(std::move(imported), texture);
				} else {
					resource = org::PixelBuffer::CreateShared(texture);
				}
			} else {
				throw std::runtime_error("CS ORG host currently supports buffers and Texture2D resources");
			}
			generation->graph->RegisterResource(org::ResourceIdentifier{ definition.name }, resource);
			generation->host.resources.emplace(definition.handle, std::move(resource));
			if (definition.desc.lifetime == CS_DX12_RESOURCE_CS_IMPORTED)
				generation->host.d3d11ProducedImports.insert(definition.handle);
		}
		auto extension = std::make_unique<EpochExtension>();
		generation->epochExtension = extension.get();
		extension->SetFrame(EpochState{ .host = &generation->host });
		extension->SetWorkItems(std::move(workItems));
		generation->graph->RegisterExtension(std::move(extension), "cs.dx12.epoch");
		generation->graph->Setup();
		generation->graph->GetMemorySnapshotProvider().SetProvider(org::memory::CreateECSMemorySnapshotProvider());
		generation->graph->CompileStructural();
		return generation;
	}

	std::unique_ptr<GraphGeneration> active;
	std::deque<std::pair<uint64_t, std::unique_ptr<GraphGeneration>>> retired;
#if defined(CS_HAS_ORG_MODULE_SERVICES)
	std::unique_ptr<org::services::FrameUploadArena> uploads;
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
	std::unique_ptr<org::services::ShaderCompiler> shaderCompiler;
#endif
	std::unique_ptr<org::services::PipelineService> pipelines;
#endif
	rhi::Device device{};
};

DX12GraphHost::DX12GraphHost(std::unique_ptr<Impl> implementation) : impl(std::move(implementation)) {}
DX12GraphHost::~DX12GraphHost() = default;

std::unique_ptr<DX12GraphHost> DX12GraphHost::Create(rhi::Device device)
{
	return std::unique_ptr<DX12GraphHost>(new DX12GraphHost(std::make_unique<Impl>(device)));
}

void DX12GraphHost::SetStructuralDefinition(std::vector<ResourceDefinition> resources, std::vector<WorkItem> workItems)
{
	auto candidate = impl->Compile(std::move(resources), std::move(workItems));
	if (impl->active) impl->retired.emplace_back(impl->active->lastCompletion, std::move(impl->active));
	impl->active = std::move(candidate);
}

void DX12GraphHost::Execute(
	uint32_t frameIndex,
	uint64_t frameFenceValue,
	rhi::Timeline readyTimeline,
	uint64_t readyValue,
	rhi::Timeline completeTimeline,
	uint64_t completeValue,
	const CSDX12FrameInfo& frame)
{
	EpochState state{}; state.readyTimeline = readyTimeline; state.readyValue = readyValue;
	state.completeTimeline = completeTimeline; state.completeValue = completeValue; state.frame = frame;
	state.host = &impl->active->host;
	impl->active->epochExtension->SetFrame(std::move(state));
	org::UpdateExecutionContext update{};
	update.frameIndex = frameIndex;
	update.frameFenceValue = frameFenceValue;
	impl->active->graph->Update(update, impl->device);
	org::PassExecutionContext execute{};
	execute.device = impl->device;
	execute.frameIndex = frameIndex;
	execute.frameFenceValue = frameFenceValue;
	execute.externalTimelineBindings.push_back({ kD3D11ReadyBinding, { readyTimeline, readyValue } });
	impl->active->graph->Execute(execute);
	impl->active->lastCompletion = completeValue;
}

void DX12GraphHost::Retire(uint64_t completedValue) noexcept {
#if defined(CS_HAS_ORG_MODULE_SERVICES)
	impl->uploads->Retire(completedValue);
	impl->pipelines->PublishReady(impl->active ? impl->active->lastCompletion : completedValue);
	impl->pipelines->Retire(completedValue);
#endif
	while (!impl->retired.empty() && impl->retired.front().first <= completedValue) impl->retired.pop_front();
}
uint64_t DX12GraphHost::UploadBytesInFlight() const noexcept {
#if defined(CS_HAS_ORG_MODULE_SERVICES)
	return impl->uploads->BytesInFlight();
#else
	return 0;
#endif
}
org::services::ShaderCompiler* DX12GraphHost::GetShaderCompiler() noexcept {
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	return impl->shaderCompiler.get();
#else
	return nullptr;
#endif
}
org::services::PipelineService* DX12GraphHost::GetPipelineService() noexcept {
#if defined(CS_HAS_ORG_MODULE_SERVICES)
	return impl->pipelines.get();
#else
	return nullptr;
#endif
}

#include "DX12GraphHost.h"

#include <Render/RenderGraph/RenderGraph.h>
#include <Render/Runtime/RuntimeDevice.h>
#include <Render/Runtime/OpenRenderGraphSettings.h>
#include <Render/MemoryIntrospectionBackend.h>
#include <RenderPasses/Base/ComputePass.h>
#include <Interfaces/IDynamicDeclaredResources.h>
#include <rhi_interop_dx12.h>
#if defined(CS_HAS_ORG_MODULE_SERVICES)
#include <ORGModuleServices/FrameUploadArena.h>
#include <ORGModuleServices/ShaderCompiler.h>
#include <ORGModuleServices/PipelineService.h>
#endif
#include <deque>
#include <filesystem>

namespace
{
	struct EpochState
	{
		rhi::Timeline readyTimeline{};
		uint64_t readyValue{};
		rhi::Timeline completeTimeline{};
		uint64_t completeValue{};
		CSDX12FrameInfo frame{};
		std::vector<DX12GraphHost::WorkItem> workItems;
		void* uploads{};
	};

	CSDX12Status CS_DX12_GRAPH_CALL UnsupportedResourceLookup(const CSDX12ExecutionContext*, CSDX12ResourceHandle, void** out)
	{
		if (out) *out = nullptr;
		return CS_DX12_E_UNSUPPORTED_CAPABILITY;
	}

	CSDX12Status CS_DX12_GRAPH_CALL AllocateUpload(const CSDX12ExecutionContext* context, uint64_t size, uint64_t alignment, CSDX12UploadAllocation* out)
	{
#if defined(CS_HAS_ORG_MODULE_SERVICES)
		if (!context || !context->frame || !context->hostContext || !out || !size) return CS_DX12_E_INVALID_ARGUMENT;
		try {
			auto allocation = static_cast<org::services::FrameUploadArena*>(context->hostContext)->Allocate(
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
			auto allocation = static_cast<org::services::FrameUploadArena*>(context->hostContext)->AllocateDescriptors(
				heapType, count, context->frame->completionValue);
			*out = { allocation.cpuHandle, allocation.gpuHandle, allocation.descriptorSize, count, allocation.heap };
			return CS_DX12_OK;
		} catch (...) { return CS_DX12_E_INTERNAL; }
#else
		(void)context; (void)heapType; (void)count; (void)out;
		return CS_DX12_E_UNSUPPORTED_CAPABILITY;
#endif
	}

	class EpochPass final : public org::ComputePass
	{
	public:
		EpochPass(std::shared_ptr<EpochState> state, size_t workIndex) : state(std::move(state)), workIndex(workIndex) {}
		void Setup() override {}
		void Cleanup() override {}
		void DeclareResourceUsages(org::ComputePassBuilder* builder) override
		{
			(void)builder;
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
			execution.hostContext = state->uploads;
			execution.GetResource = &UnsupportedResourceLookup;
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
			if (begin) builder->WithExternalWaitBeforeTransitions(state->readyTimeline, state->readyValue);
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
				auto desc = org::RenderGraph::ExternalPassDesc::Compute(item.name, std::make_shared<EpochPass>(state, i)).At(std::move(point));
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
		active = Compile({});
	}

	~Impl()
	{
		active.reset();
		retired.clear();
		org::runtime::ShutdownRuntimeDevice();
	}

	std::unique_ptr<GraphGeneration> Compile(std::vector<DX12GraphHost::WorkItem> workItems)
	{
		auto generation = std::make_unique<GraphGeneration>();
		generation->graph = std::make_unique<org::RenderGraph>(device);
		auto extension = std::make_unique<EpochExtension>();
		generation->epochExtension = extension.get();
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

void DX12GraphHost::SetStructuralWorkItems(std::vector<WorkItem> workItems)
{
	auto candidate = impl->Compile(std::move(workItems));
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
#if defined(CS_HAS_ORG_MODULE_SERVICES)
	state.uploads = impl->uploads.get();
#endif
	impl->active->epochExtension->SetFrame(std::move(state));
	org::UpdateExecutionContext update{};
	update.frameIndex = frameIndex;
	update.frameFenceValue = frameFenceValue;
	impl->active->graph->Update(update, impl->device);
	org::PassExecutionContext execute{};
	execute.device = impl->device;
	execute.frameIndex = frameIndex;
	execute.frameFenceValue = frameFenceValue;
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

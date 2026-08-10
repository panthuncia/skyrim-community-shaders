#include "ClusteredLightingExtension.h"

#include "LightCulling.h"
#include "Features/DeferredRendering.h"
#include "Globals.h"

#include <Render/PassBuilders.h>
#include <Render/Runtime/DescriptorServiceAccess.h>
#include <Render/Runtime/UploadServiceAccess.h>
#include <RenderPasses/Base/ComputePass.h>
#include <Resources/Buffers/Buffer.h>

namespace
{
	constexpr uint32_t kLightsPerPage = 12;
	constexpr uint32_t kPagesPerCluster = 10;
	const org::ResourceIdentifier kLights{ "community-shaders.clustered-lighting.lights" };
	const org::ResourceIdentifier kContexts{ "community-shaders.clustered-lighting.contexts" };
	const org::ResourceIdentifier kMaterials{ "community-shaders.clustered-lighting.pbr-materials" };
	const org::ResourceIdentifier kClusters{ "community-shaders.clustered-lighting.clusters" };
	const org::ResourceIdentifier kPages{ "community-shaders.clustered-lighting.pages" };
	const org::ResourceIdentifier kPageCounter{ "community-shaders.clustered-lighting.page-counter" };
	const org::ResourceIdentifier kDiagnostics{ "community-shaders.clustered-lighting.diagnostics" };

	struct Constants
	{
		Matrix projectionInverse;
		Matrix view;
		uint32_t grid[4]{};
		float screen[2]{};
		float nearPlane{};
		float farPlane{};
		uint32_t lightCount{};
		uint32_t contextCount{};
		uint32_t pageCapacity{};
		uint32_t materialCount{};
		uint32_t lightsIndex{ UINT32_MAX };
		uint32_t contextsIndex{ UINT32_MAX };
		uint32_t materialsIndex{ UINT32_MAX };
		uint32_t clustersIndex{ UINT32_MAX };
		uint32_t pagesIndex{ UINT32_MAX };
		uint32_t pageCounterIndex{ UINT32_MAX };
		uint32_t diagnosticsIndex{ UINT32_MAX };
	};
	static_assert(sizeof(Constants) % 4 == 0 && sizeof(Constants) / 4 <= 64);

	class PassBase : public org::ComputePass
	{
	public:
		explicit PassBase(ClusteredLightingExtension& extension) : extension_(extension) {}
		void Cleanup() override {}
	protected:
		Constants MakeConstants() const {
			Constants constants{};
			if (const auto snapshot = extension_.Snapshot()) {
				constants.projectionInverse = snapshot->projectionInverse;
				constants.view = snapshot->cameraView;
				constants.grid[0] = (snapshot->renderWidth + 63) / 64;
				constants.grid[1] = (snapshot->renderHeight + 63) / 64;
				constants.grid[2] = 32;
				constants.screen[0] = static_cast<float>(snapshot->renderWidth);
				constants.screen[1] = static_cast<float>(snapshot->renderHeight);
				constants.nearPlane = snapshot->nearPlane;
				constants.farPlane = snapshot->farPlane;
				constants.lightCount = static_cast<uint32_t>(snapshot->lights.size());
				constants.contextCount = static_cast<uint32_t>(snapshot->contexts.size());
				constants.materialCount = static_cast<uint32_t>(snapshot->pbrMaterials.size());
			}
			constants.pageCapacity = extension_.PageCapacity();
			return constants;
		}
		void Dispatch(org::PassExecutionContext& context, rhi::PipelineHandle pipeline, Constants& constants,
			uint32_t x, uint32_t y, uint32_t z) {
			const auto layout = extension_.Owner().GetLayout();
			if (!pipeline.valid() || !layout.valid())
				throw std::runtime_error("Clustered-lighting native pipeline is unavailable");
			auto heap = org::runtime::GetActiveSRVDescriptorHeap();
			if (!heap) throw std::runtime_error("Clustered-lighting descriptor heap is unavailable");
			context.commandList.SetDescriptorHeaps(heap.GetHandle(), std::nullopt);
			context.commandList.BindLayout(layout);
			context.commandList.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0,
				sizeof(constants) / 4, reinterpret_cast<const std::uint32_t*>(&constants));
			context.commandList.BindPipeline(pipeline);
			context.commandList.Dispatch(x, y, z);
		}
		ClusteredLightingExtension& extension_;
	};

	class ClearPass final : public PassBase
	{
	public: using PassBase::PassBase;
		void Setup() override { RegisterUAV(kPageCounter); RegisterUAV(kDiagnostics); }
		void Update(const org::UpdateExecutionContext&) override { extension_.UpdateSnapshot(); }
		void DeclareResourceUsages(org::ComputePassBuilder* builder) override {
			builder->WithUnorderedAccessClear(kPageCounter, kDiagnostics);
		}
		org::PassReturn Execute(org::PassExecutionContext& context) override {
			auto constants = MakeConstants();
			constants.pageCounterIndex = m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(kPageCounter, false);
			constants.diagnosticsIndex = m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(kDiagnostics, false);
			Dispatch(context, extension_.Owner().GetClearPipeline(), constants, 1, 1, 1); return {};
		}
	};

	class BuildClustersPass final : public PassBase
	{
	public: using PassBase::PassBase;
		void Setup() override { RegisterUAV(kClusters); }
		void DeclareResourceUsages(org::ComputePassBuilder* builder) override { builder->WithUnorderedAccess(kClusters); }
		org::PassReturn Execute(org::PassExecutionContext& context) override {
			auto constants = MakeConstants();
			constants.clustersIndex = m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(kClusters, false);
			Dispatch(context, extension_.Owner().GetClusterPipeline(), constants,
				constants.grid[0], constants.grid[1], constants.grid[2]); return {};
		}
	};

	class CullLightsPass final : public PassBase
	{
	public: using PassBase::PassBase;
		void Setup() override {
			RegisterSRV(kLights); RegisterUAV(kClusters); RegisterUAV(kPages);
			RegisterUAV(kPageCounter); RegisterUAV(kDiagnostics);
		}
		void DeclareResourceUsages(org::ComputePassBuilder* builder) override {
			builder->WithShaderResource(kLights).WithUnorderedAccess(kClusters, kPages, kPageCounter, kDiagnostics);
		}
		org::PassReturn Execute(org::PassExecutionContext& context) override {
			auto constants = MakeConstants();
			constants.lightsIndex = m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(kLights, false);
			constants.clustersIndex = m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(kClusters, false);
			constants.pagesIndex = m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(kPages, false);
			constants.pageCounterIndex = m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(kPageCounter, false);
			constants.diagnosticsIndex = m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(kDiagnostics, false);
			const auto total = constants.grid[0] * constants.grid[1] * constants.grid[2];
			Dispatch(context, extension_.Owner().GetCullPipeline(), constants, (total + 127) / 128, 1, 1);
			if (auto snapshot = extension_.Snapshot())
				globals::features::deferredRendering.RetainSubmittedFrame(std::move(snapshot), context.frameFenceValue);
			return {};
		}
	};
}

ClusteredLightingExtension::ClusteredLightingExtension(DX12LightCulling& owner) : owner_(owner)
{
	const auto snapshot = globals::features::deferredRendering.GetFrameSnapshot();
	const uint32_t width = snapshot ? (std::max)(1u, snapshot->renderWidth) : 1920u;
	const uint32_t height = snapshot ? (std::max)(1u, snapshot->renderHeight) : 1080u;
	const uint32_t clusterCount = ((width + 63) / 64) * ((height + 63) / 64) * 32;
	const uint32_t pageCount = clusterCount * kPagesPerCluster;
	pageCapacity_ = pageCount;
	auto create = [&](const org::ResourceIdentifier& id, uint32_t count, uint32_t stride, bool unordered) {
		auto resource = org::Buffer::CreateUnmaterializedStructuredBuffer(
			count, stride, unordered, false, false, rhi::HeapType::DeviceLocal);
		resource->SetName(id.name);
		resources_.emplace(id, std::move(resource));
	};
	create(kLights, DeferredRendering::MAX_LIGHTS, sizeof(DeferredRendering::LightData), false);
	create(kContexts, DeferredRendering::INVALID_CONTEXT, sizeof(DeferredRendering::LightingContext), false);
	create(kMaterials, CS::Deferred::kMaxPBRMaterials, sizeof(CS::Deferred::PBRMaterialRecord), false);
	create(kClusters, clusterCount, 48, true);
	create(kPages, pageCount, 56, true);
	create(kPageCounter, 1, sizeof(uint32_t), true);
	create(kDiagnostics, 4, sizeof(uint32_t), true);
}

void ClusteredLightingExtension::PrepareForBuild(org::RenderGraph& graph) {
	graph.RegisterProvider(this);
	for (const auto& key : GetSupportedKeys()) resources_[key] = graph.RequestResourcePtr(key);
}

void ClusteredLightingExtension::GatherStructuralPasses(
	org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& out)
{
	out.push_back(org::RenderGraph::ExternalPassDesc::Compute(
		"community-shaders.clustered-lighting.clear", std::make_shared<ClearPass>(*this))
		.At(org::RenderGraph::ExternalInsertPoint::After("cs.gbuffer.ready")).PreferQueue(org::QueueKind::Compute));
	out.push_back(org::RenderGraph::ExternalPassDesc::Compute(
		"community-shaders.clustered-lighting.build-clusters", std::make_shared<BuildClustersPass>(*this))
		.At(org::RenderGraph::ExternalInsertPoint::After("community-shaders.clustered-lighting.clear")).PreferQueue(org::QueueKind::Compute));
	auto cullPoint = org::RenderGraph::ExternalInsertPoint::After("community-shaders.clustered-lighting.build-clusters");
	cullPoint.AlsoBefore("cs.deferred-lighting.begin");
	out.push_back(org::RenderGraph::ExternalPassDesc::Compute(
		"community-shaders.clustered-lighting.cull-lights", std::make_shared<CullLightsPass>(*this))
		.At(std::move(cullPoint)).PreferQueue(org::QueueKind::Compute));
}

std::shared_ptr<org::Resource> ClusteredLightingExtension::ProvideResource(const org::ResourceIdentifier& key)
{
	if (const auto found = resources_.find(key); found != resources_.end()) return found->second;
	return {};
}

std::vector<org::ResourceIdentifier> ClusteredLightingExtension::GetSupportedKeys()
{
	return { kLights, kContexts, kMaterials, kClusters, kPages, kPageCounter, kDiagnostics };
}

void ClusteredLightingExtension::UpdateSnapshot()
{
	snapshot_ = globals::features::deferredRendering.GetFrameSnapshot();
	if (!snapshot_) return;
	auto upload = [&](const auto& values, const org::ResourceIdentifier& id) {
		if (!values.empty()) BUFFER_UPLOAD(values.data(), values.size() * sizeof(values[0]),
			org::runtime::UploadTarget::FromShared(resources_.at(id)), 0);
	};
	upload(snapshot_->lights, kLights); upload(snapshot_->contexts, kContexts); upload(snapshot_->pbrMaterials, kMaterials);
}

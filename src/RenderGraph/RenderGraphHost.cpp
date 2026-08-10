#include "RenderGraphHost.h"
#include "NativeRenderGraphRegistry.h"
#include "RenderGraphExecutionContext.h"

#include <Render/RenderGraph/RenderGraph.h>
#include <Render/Runtime/RuntimeDevice.h>
#include <Render/Runtime/OpenRenderGraphSettings.h>
#include <Render/Runtime/IUploadService.h>
#include <Render/Runtime/UploadServiceAccess.h>
#include <Render/Runtime/DescriptorServiceAccess.h>
#include <Render/MemoryIntrospectionBackend.h>
#include <RenderPasses/Base/ComputePass.h>
#include <RenderPasses/Base/RenderPass.h>
#include <RenderPasses/Base/CopyPass.h>
#include <Interfaces/IDynamicDeclaredResources.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/DynamicResource.h>
#include <Resources/GloballyIndexedResource.h>
#include <Resources/PixelBuffer.h>
#if defined(CS_HAS_ORG_MODULE_SERVICES)
#include <ORGModuleServices/ShaderCompiler.h>
#include <ORGModuleServices/PipelineService.h>
#endif
#include <deque>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace
{
	class ActiveGraphServices final
	{
	public:
		ActiveGraphServices(org::runtime::IUploadService* uploads,
			org::runtime::IDescriptorService* descriptors) noexcept
			: previousUploads_(org::runtime::GetActiveUploadService()),
			  previousDescriptors_(org::runtime::GetActiveDescriptorService())
		{
			org::runtime::SetActiveUploadService(uploads);
			org::runtime::SetActiveDescriptorService(descriptors);
		}
		~ActiveGraphServices()
		{
			org::runtime::SetActiveDescriptorService(previousDescriptors_);
			org::runtime::SetActiveUploadService(previousUploads_);
		}
	private:
		org::runtime::IUploadService* previousUploads_{};
		org::runtime::IDescriptorService* previousDescriptors_{};
	};

	uint16_t FormatChannels(CSRGFormat format) noexcept
	{
		switch (format) {
		case CS_RG_FORMAT_R32G32B32_TYPELESS: case CS_RG_FORMAT_R32G32B32_FLOAT:
		case CS_RG_FORMAT_R32G32B32_UINT: case CS_RG_FORMAT_R32G32B32_SINT:
		case CS_RG_FORMAT_R11G11B10_FLOAT: return 3;
		case CS_RG_FORMAT_R32G32_TYPELESS: case CS_RG_FORMAT_R32G32_FLOAT:
		case CS_RG_FORMAT_R32G32_UINT: case CS_RG_FORMAT_R32G32_SINT:
		case CS_RG_FORMAT_R16G16_TYPELESS: case CS_RG_FORMAT_R16G16_FLOAT:
		case CS_RG_FORMAT_R16G16_UNORM: case CS_RG_FORMAT_R16G16_UINT:
		case CS_RG_FORMAT_R16G16_SNORM: case CS_RG_FORMAT_R16G16_SINT:
		case CS_RG_FORMAT_R8G8_TYPELESS: case CS_RG_FORMAT_R8G8_UNORM:
		case CS_RG_FORMAT_R8G8_UINT: case CS_RG_FORMAT_R8G8_SNORM: case CS_RG_FORMAT_R8G8_SINT:
		case CS_RG_FORMAT_BC5_TYPELESS: case CS_RG_FORMAT_BC5_UNORM: case CS_RG_FORMAT_BC5_SNORM: return 2;
		case CS_RG_FORMAT_R32_TYPELESS: case CS_RG_FORMAT_D32_FLOAT: case CS_RG_FORMAT_R32_FLOAT:
		case CS_RG_FORMAT_R32_UINT: case CS_RG_FORMAT_R32_SINT:
		case CS_RG_FORMAT_R16_TYPELESS: case CS_RG_FORMAT_R16_FLOAT: case CS_RG_FORMAT_R16_UNORM:
		case CS_RG_FORMAT_R16_UINT: case CS_RG_FORMAT_R16_SNORM: case CS_RG_FORMAT_R16_SINT:
		case CS_RG_FORMAT_R8_TYPELESS: case CS_RG_FORMAT_R8_UNORM: case CS_RG_FORMAT_R8_UINT:
		case CS_RG_FORMAT_R8_SNORM: case CS_RG_FORMAT_R8_SINT:
		case CS_RG_FORMAT_BC4_TYPELESS: case CS_RG_FORMAT_BC4_UNORM: case CS_RG_FORMAT_BC4_SNORM: return 1;
		default: return 4;
		}
	}

	struct ExecutionHost
	{
		org::runtime::IUploadService* graphUploads{};
		std::unordered_map<uint64_t, std::shared_ptr<org::Resource>> resources;
		std::unordered_map<uint64_t, org::ResourceIdentifier> resourceIdentifiers;
	};
	constexpr org::ExternalTimelineBinding kD3D11ReadyBinding = 1;

	struct FrontendState
	{
		rhi::Timeline readyTimeline{};
		uint64_t readyValue{};
		rhi::Timeline completeTimeline{};
		uint64_t completeValue{};
		RenderGraphHost::FrameInfo frame{};
		std::vector<RenderGraphRegistry::Pass> genericPasses;
		std::vector<RenderGraphRegistry::Resource> genericResources;
		ExecutionHost* host{};
		rhi::Device device{};
		org::runtime::IDescriptorService* descriptorService{};
	};

	org::ResourcePtrAndRange GenericResourceRange(const RenderGraphRegistry::Access& access,
		const ExecutionHost& host)
	{
		const auto resource = host.resources.find(access.resource);
		if (resource == host.resources.end() || !resource->second)
			throw std::runtime_error("Missing ORG resource for generic pass access");
		org::RangeSpec range{};
		range.mipLower = { org::BoundType::From, access.range.firstMip };
		range.mipUpper = access.range.mipCount == UINT32_MAX ? org::Bound{ org::BoundType::All, 0 } :
			org::Bound{ org::BoundType::UpTo, access.range.firstMip + access.range.mipCount - 1 };
		range.sliceLower = { org::BoundType::From, access.range.firstArraySlice };
		range.sliceUpper = access.range.arraySize == UINT32_MAX ? org::Bound{ org::BoundType::All, 0 } :
			org::Bound{ org::BoundType::UpTo, access.range.firstArraySlice + access.range.arraySize - 1 };
		return { resource->second, range };
	}

	void DeclareGeneric(org::RenderPassBuilder* builder, const RenderGraphRegistry::Pass& pass, const FrontendState& state)
	{
		for (const auto& domain : pass.featureDomains) builder->WithActiveFeatureDomain(domain);
		for (const auto& access : pass.accesses) {
			auto resource = GenericResourceRange(access, *state.host);
			switch (access.kind) {
			case CS_RG_ACCESS_SHADER_RESOURCE: builder->WithShaderResource(resource); break;
			case CS_RG_ACCESS_CONSTANT_BUFFER: builder->WithConstantBuffer(resource); break;
			case CS_RG_ACCESS_UNORDERED_ACCESS: builder->WithUnorderedAccess(resource); break;
			case CS_RG_ACCESS_UNORDERED_ACCESS_CLEAR: builder->WithUnorderedAccessClear(resource); break;
			case CS_RG_ACCESS_RENDER_TARGET: builder->WithRenderTarget(resource); break;
			case CS_RG_ACCESS_RENDER_TARGET_CLEAR: builder->WithRenderTargetClear(resource); break;
			case CS_RG_ACCESS_DEPTH_READ: builder->WithDepthRead(resource); break;
			case CS_RG_ACCESS_DEPTH_READ_WRITE: builder->WithDepthReadWrite(resource); break;
			case CS_RG_ACCESS_DEPTH_STENCIL_CLEAR: builder->WithDepthStencilClear(resource); break;
			case CS_RG_ACCESS_COPY_SOURCE: builder->WithCopySource(resource); break;
			case CS_RG_ACCESS_COPY_DESTINATION: builder->WithCopyDest(resource); break;
			case CS_RG_ACCESS_INDIRECT_ARGUMENT: builder->WithIndirectArguments(resource); break;
			case CS_RG_ACCESS_INDEX_BUFFER: builder->WithIndexBuffer(resource); break;
			case CS_RG_ACCESS_LEGACY_INTEROP: builder->WithLegacyInterop(resource); break;
			default: throw std::runtime_error("Unsupported render-pass resource access");
			}
		}
	}

	void DeclareGeneric(org::ComputePassBuilder* builder, const RenderGraphRegistry::Pass& pass, const FrontendState& state)
	{
		for (const auto& domain : pass.featureDomains) builder->WithActiveFeatureDomain(domain);
		for (const auto& access : pass.accesses) {
			auto resource = GenericResourceRange(access, *state.host);
			switch (access.kind) {
			case CS_RG_ACCESS_SHADER_RESOURCE: builder->WithShaderResource(resource); break;
			case CS_RG_ACCESS_CONSTANT_BUFFER: builder->WithConstantBuffer(resource); break;
			case CS_RG_ACCESS_UNORDERED_ACCESS: builder->WithUnorderedAccess(resource); break;
			case CS_RG_ACCESS_UNORDERED_ACCESS_CLEAR: builder->WithUnorderedAccessClear(resource); break;
			case CS_RG_ACCESS_INDIRECT_ARGUMENT: builder->WithIndirectArguments(resource); break;
			case CS_RG_ACCESS_LEGACY_INTEROP: builder->WithLegacyInterop(resource); break;
			default: throw std::runtime_error("Unsupported compute-pass resource access");
			}
		}
	}

	void DeclareGeneric(org::CopyPassBuilder* builder, const RenderGraphRegistry::Pass& pass, const FrontendState& state)
	{
		for (const auto& access : pass.accesses) {
			auto resource = GenericResourceRange(access, *state.host);
			if (access.kind == CS_RG_ACCESS_COPY_SOURCE) builder->WithCopySource(resource);
			else if (access.kind == CS_RG_ACCESS_COPY_DESTINATION) builder->WithCopyDest(resource);
			else throw std::runtime_error("Unsupported copy-pass resource access");
		}
	}

	CSRGFrameInfo GenericFrameInfo(const FrontendState& state)
	{
		return { sizeof(CSRGFrameInfo), CS_RENDER_GRAPH_API_CURRENT, state.frame.frameIndex,
			state.frame.generation, state.frame.completionValue, state.frame.frameSlot,
			state.frame.framesInFlight, state.frame.width, state.frame.height,
			state.frame.width, state.frame.height };
	}

	CSRGStatus QueueGraphBufferUpload(void* user, CSRGBinding binding, uint64_t offset,
		const void* data, uint64_t size)
	{
		auto* host = static_cast<RenderGraphExecutionContextHost*>(user);
		if (!host || !host->graphUploads || !data || !size || size > SIZE_MAX) return CS_RG_E_INVALID_ARGUMENT;
		const auto found = host->graphResourcesByBinding.find(binding);
		if (found == host->graphResourcesByBinding.end() || !found->second) return CS_RG_E_STALE_HANDLE;
		uint64_t byteSize{};
		if (!found->second->TryGetBufferByteSize(byteSize) || offset > byteSize || size > byteSize - offset)
			return CS_RG_E_INVALID_ARGUMENT;
		try {
			host->graphUploads->UploadData(data, static_cast<size_t>(size),
				org::runtime::UploadTarget::FromShared(found->second), static_cast<size_t>(offset)
#if BUILD_TYPE == BUILD_TYPE_DEBUG
				, __FILE__, __LINE__
#endif
			);
			return CS_RG_OK;
		} catch (...) { return CS_RG_E_INTERNAL; }
	}

	org::GloballyIndexedResource* ResolveGloballyIndexed(org::Resource* resource)
	{
		if (auto* dynamic = dynamic_cast<org::DynamicGloballyIndexedResource*>(resource))
			return dynamic->GetResource().get();
		return dynamic_cast<org::GloballyIndexedResource*>(resource);
	}

	void ResolveGenericDescriptor(const std::shared_ptr<FrontendState>& state,
		const RenderGraphRegistry::Access& access, org::Resource* resource, CSRGBindingInfo& info,
		RenderGraphExecutionContextHost& host, const std::unordered_map<CSRGBinding, rhi::DescriptorSlot>& shaderSlots)
	{
		if (access.viewKind == CS_RG_VIEW_NONE) return;
		if (const auto found = shaderSlots.find(access.binding); found != shaderSlots.end())
			info.descriptorIndex = found->second.index;
		info.shaderVisible = info.descriptorIndex != UINT32_MAX;
	}

	void PopulateGenericHost(const std::shared_ptr<FrontendState>& state, const RenderGraphRegistry::Pass& pass,
		RenderGraphExecutionContextHost& host,
		const std::unordered_map<CSRGBinding, rhi::DescriptorSlot>& shaderSlots)
	{
		host.graphUploads = state->host->graphUploads;
		host.uploadUser = &host;
		host.queueBufferUpload = &QueueGraphBufferUpload;
		for (const auto& access : pass.accesses) {
			const auto resource = state->host->resources.find(access.resource);
			if (resource == state->host->resources.end() || !resource->second)
				throw std::runtime_error("Generic binding has no ORG resource");
			host.graphResourcesByBinding.emplace(access.binding, resource->second);
			const auto definition = std::ranges::find_if(state->genericResources,
				[&](const RenderGraphRegistry::Resource& value) { return value.handle == access.resource; });
			if (definition == state->genericResources.end())
				throw std::runtime_error("Generic binding has no resource definition");
			CSRGBindingInfo info{ sizeof(CSRGBindingInfo), CS_RENDER_GRAPH_API_CURRENT };
			info.binding = access.binding; info.access = access.kind; info.viewKind = access.viewKind;
			info.dimension = definition->desc.dimension; info.resourceFormat = definition->desc.format;
			info.viewFormat = access.viewFormat == CS_RG_FORMAT_UNKNOWN ? definition->desc.format : access.viewFormat;
			info.width = definition->desc.width; info.height = definition->desc.height;
			info.depthOrArraySize = definition->desc.depthOrArraySize; info.mipLevels = definition->desc.mipLevels;
			info.byteSize = definition->desc.byteSize; info.descriptorIndex = UINT32_MAX;
			ResolveGenericDescriptor(state, access, resource->second.get(), info, host, shaderSlots);
			host.bindingInfo.emplace(access.binding, info);
		}
	}

	template <class PassBase>
	class GenericProxyPass : public PassBase
	{
	public:
		GenericProxyPass(std::shared_ptr<FrontendState> state, size_t index) : state_(std::move(state)), index_(index) {}
		~GenericProxyPass() override { ReleaseDescriptors(); }
		void Setup() override
		{
			const auto& pass = state_->genericPasses.at(index_);
			for (const auto& access : pass.accesses) CreateDescriptor(access);
			if (pass.prepare) Invoke(pass.prepare, "preparation");
			prepared_ = true;
		}
		void Update(const org::UpdateExecutionContext&) override
		{
			const auto& pass = state_->genericPasses.at(index_);
			if (pass.update) Invoke(pass.update, "update");
		}
		void Cleanup() override
		{
			if (cleaned_) return;
			cleaned_ = true;
			const auto& pass = state_->genericPasses.at(index_);
			if (prepared_ && pass.cleanup) try { pass.cleanup(pass.userData, state_->frame.generation); } catch (...) {}
			ReleaseDescriptors();
		}
		org::PassReturn ExecuteProxy(org::PassExecutionContext& context)
		{
			Invoke(state_->genericPasses.at(index_).execute, "execution");
			return {};
		}
	protected:
		std::shared_ptr<FrontendState> state_;
		size_t index_{};
	private:
		template <class Callback>
		void Invoke(Callback callback, const char* phase)
		{
			const auto& pass = state_->genericPasses.at(index_);
			RenderGraphExecutionContextHost host{};
			PopulateGenericHost(state_, pass, host, shaderSlots_);
			auto frame = GenericFrameInfo(*state_);
			CSRGExecutionContext execution{ sizeof(execution), CS_RENDER_GRAPH_API_CURRENT,
				CS_RG_BACKEND_NONE, 0, &frame, &host };
			CSRGStatus status = CS_RG_E_CALLBACK_FAILED;
			try { status = callback(pass.userData, &execution); } catch (...) {}
			if (status != CS_RG_OK)
				throw std::runtime_error("Generic pass " + std::string(phase) + " failed: " + pass.id);
		}
		bool prepared_{};
		bool cleaned_{};
		std::unordered_map<CSRGBinding, rhi::DescriptorSlot> shaderSlots_;
		std::unordered_map<CSRGBinding, rhi::DescriptorSlot> cpuSlots_;
		std::vector<rhi::DescriptorSlot> leasedSlots_;
		void ReleaseDescriptors() noexcept {
			if (state_ && state_->descriptorService) {
				for (const auto& slot : leasedSlots_) try { state_->descriptorService->RetireDescriptorSlot(slot); } catch (...) {}
			}
			shaderSlots_.clear(); cpuSlots_.clear(); leasedSlots_.clear();
		}

		void CreateDescriptor(const RenderGraphRegistry::Access& access)
		{
			if (access.viewKind == CS_RG_VIEW_NONE) return;
			if (!state_->descriptorService) throw std::runtime_error("ORG descriptor service unavailable");
			const auto resourceIt = state_->host->resources.find(access.resource);
			const auto definitionIt = std::ranges::find_if(state_->genericResources,
				[&](const RenderGraphRegistry::Resource& value) { return value.handle == access.resource; });
			if (resourceIt == state_->host->resources.end() || !resourceIt->second || definitionIt == state_->genericResources.end())
				throw std::runtime_error("Descriptor binding has no resolved resource");
			auto apiResource = resourceIt->second->GetAPIResource();
			auto format = access.viewFormat == CS_RG_FORMAT_UNKNOWN ?
				static_cast<rhi::Format>(definitionIt->desc.format) : static_cast<rhi::Format>(access.viewFormat);
			if ((access.viewFlags & CS_RG_VIEW_FLAG_RAW_BUFFER) != 0) format = rhi::Format::R32_Typeless;
			const uint32_t mipCount = access.range.mipCount == UINT32_MAX ?
				definitionIt->desc.mipLevels - access.range.firstMip : access.range.mipCount;
			const uint32_t arraySize = access.range.arraySize == UINT32_MAX ?
				definitionIt->desc.depthOrArraySize - access.range.firstArraySlice : access.range.arraySize;
			auto allocate = [&](rhi::DescriptorHeapType type, bool shaderVisible) {
				auto slot = state_->descriptorService->AllocateDescriptorSlot(type, shaderVisible);
				leasedSlots_.push_back(slot);
				return slot;
			};
			if (access.viewKind == CS_RG_VIEW_CONSTANT_BUFFER) {
				auto slot = allocate(rhi::DescriptorHeapType::CbvSrvUav, true);
				rhi::CbvDesc desc{ access.firstElement, access.elementCount == UINT32_MAX ?
					static_cast<uint32_t>(definitionIt->desc.byteSize - access.firstElement) : access.elementCount };
				if (rhi::Failed(state_->device.CreateConstantBufferView(slot, apiResource.GetHandle(), desc)))
					throw std::runtime_error("Failed to create exact ORG CBV");
				shaderSlots_.emplace(access.binding, slot); return;
			}
			if (access.viewKind == CS_RG_VIEW_SHADER_RESOURCE) {
				auto slot = allocate(rhi::DescriptorHeapType::CbvSrvUav, true); rhi::SrvDesc desc{}; desc.formatOverride = format;
				if (definitionIt->desc.dimension == CS_RG_RESOURCE_BUFFER) {
					const uint64_t stride = (access.viewFlags & CS_RG_VIEW_FLAG_RAW_BUFFER) ? 4u :
						access.structureByteStride ? access.structureByteStride : 1u;
					const uint64_t total = definitionIt->desc.byteSize / stride;
					const uint32_t count = access.elementCount == UINT32_MAX ?
						static_cast<uint32_t>(total - access.firstElement) : access.elementCount;
					desc.dimension = rhi::SrvDim::Buffer; desc.buffer.kind = (access.viewFlags & CS_RG_VIEW_FLAG_RAW_BUFFER) ?
						rhi::BufferViewKind::Raw : access.structureByteStride ? rhi::BufferViewKind::Structured : rhi::BufferViewKind::Typed;
					desc.buffer.firstElement = access.firstElement; desc.buffer.numElements = count;
					desc.buffer.structureByteStride = access.structureByteStride;
				} else if (access.viewDimension == CS_RG_VIEW_DIMENSION_TEXTURE_CUBE) {
					desc.dimension = rhi::SrvDim::TextureCube; desc.cube = { access.range.firstMip, mipCount, 0.0f };
				} else if (access.viewDimension == CS_RG_VIEW_DIMENSION_TEXTURE_CUBE_ARRAY) {
					desc.dimension = rhi::SrvDim::TextureCubeArray; desc.cubeArray = { access.range.firstMip, mipCount,
						access.range.firstArraySlice, arraySize / 6, 0.0f };
				} else if (arraySize > 1 || access.viewDimension == CS_RG_VIEW_DIMENSION_TEXTURE_2D_ARRAY) {
					desc.dimension = rhi::SrvDim::Texture2DArray; desc.tex2DArray = { access.range.firstMip, mipCount,
						access.range.firstArraySlice, arraySize, 0, 0.0f };
				} else { desc.dimension = rhi::SrvDim::Texture2D; desc.tex2D = { access.range.firstMip, mipCount, 0, 0.0f }; }
				if (rhi::Failed(state_->device.CreateShaderResourceView(slot, apiResource.GetHandle(), desc)))
					throw std::runtime_error("Failed to create exact ORG SRV");
				shaderSlots_.emplace(access.binding, slot); return;
			}
			if (access.viewKind == CS_RG_VIEW_UNORDERED_ACCESS) {
				auto slot = allocate(rhi::DescriptorHeapType::CbvSrvUav, true); rhi::UavDesc desc{}; desc.formatOverride = format;
				if (definitionIt->desc.dimension == CS_RG_RESOURCE_BUFFER) {
					const uint64_t stride = (access.viewFlags & CS_RG_VIEW_FLAG_RAW_BUFFER) ? 4u :
						access.structureByteStride ? access.structureByteStride : 1u;
					const uint64_t total = definitionIt->desc.byteSize / stride;
					const uint32_t count = access.elementCount == UINT32_MAX ?
						static_cast<uint32_t>(total - access.firstElement) : access.elementCount;
					desc.dimension = rhi::UavDim::Buffer; desc.buffer.kind = (access.viewFlags & CS_RG_VIEW_FLAG_RAW_BUFFER) ?
						rhi::BufferViewKind::Raw : access.structureByteStride ? rhi::BufferViewKind::Structured : rhi::BufferViewKind::Typed;
					desc.buffer.firstElement = access.firstElement; desc.buffer.numElements = count;
					desc.buffer.structureByteStride = access.structureByteStride;
				} else if (arraySize > 1 || access.viewDimension == CS_RG_VIEW_DIMENSION_TEXTURE_2D_ARRAY) {
					desc.dimension = rhi::UavDim::Texture2DArray; desc.texture2DArray = { access.range.firstMip,
						access.range.firstArraySlice, arraySize, 0 };
				} else { desc.dimension = rhi::UavDim::Texture2D; desc.texture2D = { access.range.firstMip, 0 }; }
				if (rhi::Failed(state_->device.CreateUnorderedAccessView(slot, apiResource.GetHandle(), desc)))
					throw std::runtime_error("Failed to create exact ORG UAV");
				shaderSlots_.emplace(access.binding, slot);
				if (access.kind == CS_RG_ACCESS_UNORDERED_ACCESS_CLEAR) {
					auto cpu = allocate(rhi::DescriptorHeapType::CbvSrvUav, false);
					if (rhi::Failed(state_->device.CreateUnorderedAccessView(cpu, apiResource.GetHandle(), desc)))
						throw std::runtime_error("Failed to create exact CPU ORG UAV");
					cpuSlots_.emplace(access.binding, cpu);
				}
				return;
			}
			if (access.viewKind == CS_RG_VIEW_RENDER_TARGET) {
				auto slot = allocate(rhi::DescriptorHeapType::RTV, false); rhi::RtvDesc desc{}; desc.formatOverride = format;
				desc.dimension = arraySize > 1 ? rhi::RtvDim::Texture2DArray : rhi::RtvDim::Texture2D;
				desc.range = { access.range.firstMip, 1, access.range.firstArraySlice, arraySize };
				if (rhi::Failed(state_->device.CreateRenderTargetView(slot, apiResource.GetHandle(), desc)))
					throw std::runtime_error("Failed to create exact ORG RTV");
				cpuSlots_.emplace(access.binding, slot); return;
			}
			auto slot = allocate(rhi::DescriptorHeapType::DSV, false); rhi::DsvDesc desc{}; desc.formatOverride = format;
			desc.dimension = arraySize > 1 ? rhi::DsvDim::Texture2DArray : rhi::DsvDim::Texture2D;
			desc.range = { access.range.firstMip, 1, access.range.firstArraySlice, arraySize };
			desc.readOnlyDepth = (access.viewFlags & CS_RG_VIEW_FLAG_READ_ONLY_DEPTH) != 0;
			desc.readOnlyStencil = (access.viewFlags & CS_RG_VIEW_FLAG_READ_ONLY_STENCIL) != 0;
			if (rhi::Failed(state_->device.CreateDepthStencilView(slot, apiResource.GetHandle(), desc)))
				throw std::runtime_error("Failed to create exact ORG DSV");
			cpuSlots_.emplace(access.binding, slot);
		}
	};

	class GenericRenderPass final : public GenericProxyPass<org::RenderPass>
	{
	public:
		using GenericProxyPass::GenericProxyPass;
		void DeclareResourceUsages(org::RenderPassBuilder* builder) override { DeclareGeneric(builder, state_->genericPasses.at(index_), *state_); }
		org::PassReturn Execute(org::PassExecutionContext& context) override { return ExecuteProxy(context); }
	};

	class GenericComputePass final : public GenericProxyPass<org::ComputePass>
	{
	public:
		using GenericProxyPass::GenericProxyPass;
		void DeclareResourceUsages(org::ComputePassBuilder* builder) override { DeclareGeneric(builder, state_->genericPasses.at(index_), *state_); }
		org::PassReturn Execute(org::PassExecutionContext& context) override { return ExecuteProxy(context); }
	};

	class GenericCopyPass final : public GenericProxyPass<org::CopyPass>
	{
	public:
		using GenericProxyPass::GenericProxyPass;
		void DeclareResourceUsages(org::CopyPassBuilder* builder) override { DeclareGeneric(builder, state_->genericPasses.at(index_), *state_); }
		org::PassReturn Execute(org::PassExecutionContext& context) override { return ExecuteProxy(context); }
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
		BoundaryPass(std::shared_ptr<FrontendState> state, bool begin) : state(std::move(state)), begin(begin) {}
		void Setup() override {}
		void Cleanup() override {}
		bool DeclaredResourcesChanged() const override { return true; }
		bool RequiresPassRebindAfterDeclarationRefresh() const noexcept override { return false; }
		void DeclareResourceUsages(org::ComputePassBuilder* builder) override
		{
			(void)builder;
		}
		org::PassReturn Execute(org::PassExecutionContext&) override
		{
			org::PassReturn result{};
			if (!begin) result.externalSignalsAfterCompletion.push_back({ state->completeTimeline, state->completeValue });
			return result;
		}
	private:
		std::shared_ptr<FrontendState> state;
		bool begin{};
	};

	class FrontendExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		FrontendExtension() : state(std::make_shared<FrontendState>()) {}
		void SetFrame(FrontendState value) {
			state->readyTimeline = value.readyTimeline; state->readyValue = value.readyValue;
			state->completeTimeline = value.completeTimeline; state->completeValue = value.completeValue;
			state->frame = value.frame;
			state->host = value.host;
		}
		void SetGenericPasses(std::vector<RenderGraphRegistry::Pass> value) { state->genericPasses = std::move(value); }
		void SetGenericResources(std::vector<RenderGraphRegistry::Resource> value) { state->genericResources = std::move(value); }
		void SetDescriptorContext(rhi::Device device, org::runtime::IDescriptorService& descriptors) {
			state->device = device;
			state->descriptorService = &descriptors;
		}
		std::shared_ptr<FrontendState> GetState() const { return state; }
		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& out) override {
			auto begin = org::RenderGraph::ExternalPassDesc::Compute("cs.render-graph.begin", std::make_shared<BoundaryPass>(state, true))
				.PreferQueue(org::QueueKind::Graphics).CollectStatistics(false);
			auto end = org::RenderGraph::ExternalPassDesc::Compute("cs.render-graph.end", std::make_shared<BoundaryPass>(state, false))
				.PreferQueue(org::QueueKind::Graphics).CollectStatistics(false);
			out.push_back(std::move(begin));
			constexpr const char* anchors[]{ CS_RG_ANCHOR_FRAME_BEGIN, CS_RG_ANCHOR_SHADOWS_READY,
				CS_RG_ANCHOR_GBUFFER_READY, CS_RG_ANCHOR_DEFERRED_LIGHTING_BEGIN,
				CS_RG_ANCHOR_DEFERRED_LIGHTING_END, CS_RG_ANCHOR_FRAME_END };
			const char* previous = "cs.render-graph.begin";
			for (const auto* anchor : anchors) {
				auto anchorDesc = org::RenderGraph::ExternalPassDesc::Compute(anchor, std::make_shared<AnchorPass>())
					.At(org::RenderGraph::ExternalInsertPoint::After(previous)).PreferQueue(org::QueueKind::Graphics).CollectStatistics(false);
				out.push_back(std::move(anchorDesc));
				previous = anchor;
			}
			std::string previousSerialized;
			for (size_t i = 0; i < state->genericPasses.size(); ++i) {
				const auto& item = state->genericPasses[i];
				auto point = org::RenderGraph::ExternalInsertPoint::After("cs.render-graph.begin");
				point.keepExtensionOrder = false;
				point.priority = item.priority;
				for (const auto& dependency : item.after) point.AlsoAfter(dependency);
				for (const auto& dependency : item.before) point.AlsoBefore(dependency);
				if ((item.flags & CS_RG_PASS_PARALLEL_RECORDING_SAFE) == 0 && !previousSerialized.empty())
					point.AlsoAfter(previousSerialized);
				org::RenderGraph::ExternalPassDesc desc{};
				switch (item.kind) {
				case CS_RG_PASS_RENDER: desc = org::RenderGraph::ExternalPassDesc::Render(
					item.id, std::make_shared<GenericRenderPass>(state, i)); break;
				case CS_RG_PASS_COMPUTE: desc = org::RenderGraph::ExternalPassDesc::Compute(
					item.id, std::make_shared<GenericComputePass>(state, i)); break;
				case CS_RG_PASS_COPY: desc = org::RenderGraph::ExternalPassDesc::Copy(
					item.id, std::make_shared<GenericCopyPass>(state, i)); break;
				default: throw std::runtime_error("Unknown generic pass kind");
				}
				desc.At(std::move(point));
				switch (item.queue) {
				case CS_RG_QUEUE_AUTOMATIC: desc.AutomaticQueueAssignment(); break;
				case CS_RG_QUEUE_FORCE_GRAPHICS: desc.PreferQueue(org::QueueKind::Graphics); break;
				case CS_RG_QUEUE_FORCE_COMPUTE: desc.PreferQueue(org::QueueKind::Compute); break;
				case CS_RG_QUEUE_FORCE_COPY: desc.PreferQueue(org::QueueKind::Copy); break;
				default: throw std::runtime_error("Unknown generic queue assignment");
				}
				desc.CollectStatistics((item.flags & CS_RG_PASS_DISABLE_STATISTICS) == 0)
					.GeometryPass((item.flags & CS_RG_PASS_GEOMETRY) != 0);
				if (!item.technique.empty()) desc.Technique(item.technique);
				out.push_back(std::move(desc));
				if ((item.flags & CS_RG_PASS_PARALLEL_RECORDING_SAFE) == 0) previousSerialized = item.id;
			}
			auto endPoint = org::RenderGraph::ExternalInsertPoint::After(CS_RG_ANCHOR_FRAME_END);
			endPoint.keepExtensionOrder = false;
			for (const auto& item : state->genericPasses) endPoint.AlsoAfter(item.id);
			end.At(std::move(endPoint));
			out.push_back(std::move(end));
		}
	private:
		std::shared_ptr<FrontendState> state;
	};

	class HostIOExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit HostIOExtension(org::runtime::IUploadService* uploads) : uploads_(uploads) {}
		void OnRegistryReset(org::ResourceRegistry* registry) override {
			if (uploads_) uploads_->SetUploadResolveContext({ registry, 0 });
		}
		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& out) override {
			if (uploads_) if (auto pass = uploads_->GetUploadPass())
				out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.host.uploads", std::move(pass))
					.At(org::RenderGraph::ExternalInsertPoint::Begin(-1000)).CollectStatistics(false));
		}
	private:
		org::runtime::IUploadService* uploads_{};
	};
}

class RenderGraphHost::Impl
{
public:
	struct GraphGeneration
	{
		struct NativeLifecycle { NativeRenderGraphRegistry::GenerationCallback activated; NativeRenderGraphRegistry::GenerationCallback retired; };
		std::unique_ptr<org::RenderGraph> graph;
		FrontendExtension* frontendExtension{};
		ExecutionHost host{};
		std::vector<NativeLifecycle> nativeLifecycle;
		uint64_t generation{};
		bool activationDelivered{};
		uint64_t lastCompletion{};
		~GraphGeneration() {
			if (graph) { graph->ShutdownExtensions(); graph.reset(); }
			if (activationDelivered) for (auto& lifecycle : nativeLifecycle)
				if (lifecycle.retired) try { lifecycle.retired(generation); } catch (...) {}
		}
		void Activate(uint64_t value) {
			if (activationDelivered) return;
			generation = value; activationDelivered = true;
			for (auto& lifecycle : nativeLifecycle)
				if (lifecycle.activated) try { lifecycle.activated(generation); } catch (...) {}
		}
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
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
		shaderCompiler = std::make_unique<org::services::ShaderCompiler>(std::filesystem::path("Data/SKSE/Plugins/CommunityShaders/ShaderCache/ORG"));
#endif
		pipelines = std::make_unique<org::services::PipelineService>();
#endif
		// The host has no useful graph until the registry publishes its first
		// complete candidate.  Creating an empty bootstrap generation here made
		// its resources overlap the first real generation and then retired them
		// at the first completion boundary.  Start without an active generation;
		// SetStructuralDefinition publishes the first fully prepared graph.
	}

	~Impl()
	{
		org::runtime::SetActiveDescriptorService(nullptr);
		org::runtime::SetActiveUploadService(nullptr);
		active.reset();
		retired.clear();
		org::runtime::ShutdownRuntimeDevice();
	}

	std::unique_ptr<GraphGeneration> Compile(const RenderGraphRegistry::Candidate& candidate)
	{
		auto generation = std::make_unique<GraphGeneration>();
		generation->graph = std::make_unique<org::RenderGraph>(device);
		generation->host.graphUploads = generation->graph->GetUploadService();
		generation->graph->RegisterExtension(
			std::make_unique<HostIOExtension>(generation->host.graphUploads), "cs.host.io");
		for (const auto& definition : candidate.resources) {
			std::shared_ptr<org::Resource> resource;
			if (definition.desc.dimension == CS_RG_RESOURCE_BUFFER) {
				const bool unordered = (definition.desc.allowedUsages & CS_RG_USAGE_UNORDERED_ACCESS) != 0;
					const auto heap = definition.desc.heapClass == CS_RG_HEAP_UPLOAD ? rhi::HeapType::Upload :
						definition.desc.heapClass == CS_RG_HEAP_READBACK ? rhi::HeapType::Readback : rhi::HeapType::DeviceLocal;
					std::shared_ptr<org::Buffer> buffer;
					if (definition.desc.structureByteStride) {
						const auto count = definition.desc.byteSize / definition.desc.structureByteStride;
						if (count > UINT32_MAX) throw std::runtime_error("Structured buffer element count exceeds ORG limits");
						buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(static_cast<uint32_t>(count),
							definition.desc.structureByteStride, unordered, false,
							(definition.desc.allowedUsages & CS_RG_USAGE_UNORDERED_ACCESS) != 0, heap);
					} else {
						buffer = org::Buffer::CreateSharedUnmaterialized(heap, definition.desc.byteSize, unordered);
						org::BufferBase::DescriptorRequirements requirements{};
						const bool shaderRead = (definition.desc.allowedUsages & CS_RG_USAGE_SHADER_RESOURCE) != 0;
						const bool constant = (definition.desc.allowedUsages & CS_RG_USAGE_CONSTANT_BUFFER) != 0;
						requirements.createSRV = shaderRead; requirements.createCBV = constant;
						requirements.createUAV = unordered;
						requirements.createNonShaderVisibleUAV = unordered;
						if (shaderRead) requirements.srvDesc = { .dimension = rhi::SrvDim::Buffer,
							.formatOverride = rhi::Format::R32_Typeless,
							.buffer = { .kind = rhi::BufferViewKind::Raw, .firstElement = 0,
								.numElements = static_cast<uint32_t>(definition.desc.byteSize / 4) } };
						if (unordered) requirements.uavDesc = { .dimension = rhi::UavDim::Buffer,
							.formatOverride = rhi::Format::R32_Typeless,
							.buffer = { .kind = rhi::BufferViewKind::Raw, .firstElement = 0,
								.numElements = static_cast<uint32_t>(definition.desc.byteSize / 4) } };
						if (constant) requirements.cbvDesc.byteSize = static_cast<uint32_t>(definition.desc.byteSize);
						if (shaderRead || unordered || constant) buffer->SetDescriptorRequirements(requirements);
					}
					buffer->SetName(definition.id);
					buffer->SetAllowAlias(definition.desc.allowAlias != 0);
					if (definition.desc.aliasingPool) buffer->SetAliasingPool(definition.desc.aliasingPool);
				resource = std::move(buffer);
			} else if (definition.desc.dimension == CS_RG_RESOURCE_TEXTURE_2D) {
				org::TextureDescription texture{};
				texture.imageDimensions.resize(definition.desc.mipLevels);
				uint32_t width = definition.desc.width, height = definition.desc.height;
				for (auto& dimensions : texture.imageDimensions) {
					dimensions.width = width; dimensions.height = height;
					width = (std::max)(1u, width >> 1); height = (std::max)(1u, height >> 1);
				}
				texture.format = static_cast<rhi::Format>(definition.desc.format);
				texture.channels = FormatChannels(definition.desc.format);
				texture.arraySize = definition.desc.depthOrArraySize;
				texture.isArray = texture.arraySize > 1;
				texture.hasSRV = (definition.desc.allowedUsages & CS_RG_USAGE_SHADER_RESOURCE) != 0;
				texture.hasUAV = (definition.desc.allowedUsages & CS_RG_USAGE_UNORDERED_ACCESS) != 0;
				texture.hasRTV = (definition.desc.allowedUsages & CS_RG_USAGE_RENDER_TARGET) != 0;
				texture.hasDSV = (definition.desc.allowedUsages & (CS_RG_USAGE_DEPTH_READ | CS_RG_USAGE_DEPTH_WRITE)) != 0;
				texture.allowAlias = definition.desc.allowAlias != 0;
				if (definition.desc.aliasingPool) texture.aliasingPoolID = definition.desc.aliasingPool;
				resource = org::PixelBuffer::CreateShared(texture);
			} else throw std::runtime_error("Generic ORG resources currently support buffers and Texture2D");
			const org::ResourceIdentifier identifier{ definition.id };
			generation->graph->RegisterResource(identifier, resource);
			generation->host.resources.emplace(definition.handle, std::move(resource));
			generation->host.resourceIdentifiers.emplace(definition.handle, identifier);
		}
		std::unordered_set<std::string> availableResources;
		for (const auto& definition : candidate.resources)
			availableResources.insert(definition.id);
		auto nativeExtensions = NativeRenderGraphRegistry::Get().BuildCandidate(availableResources);
		for (auto& installed : nativeExtensions) {
			generation->nativeLifecycle.push_back({ std::move(installed.activated), std::move(installed.retired) });
			generation->graph->RegisterExtension(std::move(installed.extension), installed.id);
		}
		auto extension = std::make_unique<FrontendExtension>();
		generation->frontendExtension = extension.get();
		extension->SetFrame(FrontendState{ .frame = { .generation = candidate.generation }, .host = &generation->host });
		extension->SetGenericPasses(candidate.passes);
		extension->SetGenericResources(candidate.resources);
		extension->SetDescriptorContext(device, *generation->graph->GetDescriptorService());
		generation->graph->RegisterExtension(std::move(extension), "cs.render-graph.frontend");
		ActiveGraphServices services(generation->graph->GetUploadService(),
			generation->graph->GetDescriptorService());
		// Native extensions register their symbolic providers here. ORG keeps this
		// step explicit so applications can finish installing every extension
		// before any provider resolves a cross-extension resource.
		generation->graph->PrepareExtensionsForBuild();
		generation->graph->GetMemorySnapshotProvider().SetProvider(org::memory::CreateECSMemorySnapshotProvider());
		generation->graph->CompileStructural();
		// Structural compilation gathers the extension passes. Setup must follow
		// it so those passes receive registry views, descriptor helpers, and their
		// exactly-once Setup callback before the candidate can activate.
		generation->graph->Setup();
		return generation;
	}

	std::unique_ptr<GraphGeneration> active;
	std::deque<std::pair<uint64_t, std::unique_ptr<GraphGeneration>>> retired;
#if defined(CS_HAS_ORG_MODULE_SERVICES)
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
	std::unique_ptr<org::services::ShaderCompiler> shaderCompiler;
#endif
	std::unique_ptr<org::services::PipelineService> pipelines;
#endif
	rhi::Device device{};
};

RenderGraphHost::RenderGraphHost(std::unique_ptr<Impl> implementation) : impl(std::move(implementation)) {}
RenderGraphHost::~RenderGraphHost() = default;

std::unique_ptr<RenderGraphHost> RenderGraphHost::Create(rhi::Device device)
{
	return std::unique_ptr<RenderGraphHost>(new RenderGraphHost(std::make_unique<Impl>(device)));
}

void RenderGraphHost::SetStructuralDefinition(const RenderGraphRegistry::Candidate& genericCandidate)
{
	auto candidate = impl->Compile(genericCandidate);
	if (impl->active) impl->retired.emplace_back(impl->active->lastCompletion, std::move(impl->active));
	impl->active = std::move(candidate);
}

void RenderGraphHost::Execute(
	uint32_t frameIndex,
	uint64_t frameFenceValue,
	rhi::Timeline readyTimeline,
	uint64_t readyValue,
	rhi::Timeline completeTimeline,
	uint64_t completeValue,
	const FrameInfo& frame)
{
	FrontendState state{}; state.readyTimeline = readyTimeline; state.readyValue = readyValue;
	state.completeTimeline = completeTimeline; state.completeValue = completeValue; state.frame = frame;
	state.host = &impl->active->host;
	impl->active->Activate(frame.generation);
	impl->active->frontendExtension->SetFrame(std::move(state));
	// ORG may continue recording batches on worker threads after Update/Execute
	// returns. Publish the active generation's services until the next generation
	// replaces them (or shutdown), rather than clearing them at function exit.
	org::runtime::SetActiveUploadService(impl->active->graph->GetUploadService());
	org::runtime::SetActiveDescriptorService(impl->active->graph->GetDescriptorService());
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

void RenderGraphHost::Retire(uint64_t completedValue) noexcept {
#if defined(CS_HAS_ORG_MODULE_SERVICES)
	impl->pipelines->PublishReady(impl->active ? impl->active->lastCompletion : completedValue);
	impl->pipelines->Retire(completedValue);
#endif
	while (!impl->retired.empty() && impl->retired.front().first <= completedValue) impl->retired.pop_front();
}
org::services::ShaderCompiler* RenderGraphHost::GetShaderCompiler() noexcept {
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	return impl->shaderCompiler.get();
#else
	return nullptr;
#endif
}
org::services::PipelineService* RenderGraphHost::GetPipelineService() noexcept {
#if defined(CS_HAS_ORG_MODULE_SERVICES)
	return impl->pipelines.get();
#else
	return nullptr;
#endif
}

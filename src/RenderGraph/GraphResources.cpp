#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/GraphResources.h"

#	include "Globals.h"
#	include "RenderGraph/Backend.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "Utils/D3D.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/PassBuilders.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <Resources/TextureDescription.h>
#	include <Resources/VersionedTexture.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <atomic>
#	include <bit>
#	include <vector>

namespace GraphResources
{
	namespace
	{
		// A texture's description, 2D or 3D alike.
		struct Shape
		{
			D3D11_RESOURCE_DIMENSION dimension;
			UINT width, height;
			UINT depthOrArraySize;  // depth of a 3D texture, array size of a 2D one
			UINT mips;              // 0: the whole chain, as in D3D11
			DXGI_FORMAT format;
			UINT samples;
			UINT bindFlags;
			UINT miscFlags;
			D3D11_USAGE usage;
			UINT cpuAccess;
		};

		struct Created
		{
			std::shared_ptr<org::Resource> graph;
			winrt::com_ptr<ID3D11Resource> d3d11;
		};

		bool Is3D(const Shape& a_shape) { return a_shape.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D; }

		bool IsDepth(const Shape& a_shape) { return (a_shape.bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0; }

		/** @brief The formats of a texture's image and views: its own for each, or a typeless depth texture's (Texture2D's rule). */
		struct Formats
		{
			DXGI_FORMAT image = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT srv = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;  // depth textures only
		};

		/** @brief a_shape's formats; all unknown for a depth texture whose format is not one of the typeless depth formats. */
		Formats FormatsOf(const Shape& a_shape)
		{
			if (!IsDepth(a_shape))
				return { a_shape.format, a_shape.format, DXGI_FORMAT_UNKNOWN };
			switch (a_shape.format) {
			case DXGI_FORMAT_R16_TYPELESS:
				return { DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_D16_UNORM };
			case DXGI_FORMAT_R24G8_TYPELESS:
				return { DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT };
			case DXGI_FORMAT_R32_TYPELESS:
				return { DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT };
			case DXGI_FORMAT_R32G8X24_TYPELESS:
				return { DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT };
			default:
				return {};
			}
		}

		UINT MipCount(const Shape& a_shape)
		{
			if (a_shape.mips)
				return a_shape.mips;
			const UINT largest = (std::max)({ a_shape.width, a_shape.height, Is3D(a_shape) ? a_shape.depthOrArraySize : 1u, 1u });
			return static_cast<UINT>(std::bit_width(largest));
		}

		/**
		 * @brief The graph's description of the texture. A PixelBuffer reads its mips from one entry per slice and mip (slice-major),
		 * an external texture from one entry per mip.
		 */
		org::TextureDescription Describe(const Shape& a_shape, bool a_external)
		{
			const bool cube = (a_shape.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
			const UINT slices = Is3D(a_shape) ? 1u : (std::max)(a_shape.depthOrArraySize, 1u);
			const UINT mips = MipCount(a_shape);
			const auto formats = FormatsOf(a_shape);
			const auto format = rhi::helpers::ToRHI(formats.image);
			org::TextureDescription desc{};
			desc.type = Is3D(a_shape) ? rhi::ResourceType::Texture3D : rhi::ResourceType::Texture2D;
			desc.format = format;
			desc.isCubemap = cube;
			desc.arraySize = cube ? slices / 6 : slices;
			desc.isArray = slices > (cube ? 6u : 1u);
			desc.depth = Is3D(a_shape) ? a_shape.depthOrArraySize : 1u;
			desc.sampleCount = static_cast<std::uint8_t>((std::max)(a_shape.samples, 1u));
			desc.hasSRV = true;
			desc.srvFormat = rhi::helpers::ToRHI(formats.srv);
			desc.hasUAV = (a_shape.bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
			desc.uavFormat = format;
			// A CPU-side UAV too, so a pass can clear it (PassBuilder::UnorderedAccessClear), as D3D11 clears its UAVs.
			desc.hasNonShaderVisibleUAV = desc.hasUAV;
			desc.hasRTV = (a_shape.bindFlags & D3D11_BIND_RENDER_TARGET) != 0;
			desc.rtvFormat = format;
			desc.hasDSV = IsDepth(a_shape);
			desc.dsvFormat = format;
			if (desc.hasDSV)
				desc.channels = 1;
			for (UINT slice = 0; slice < (a_external ? 1u : slices); ++slice)
				for (UINT mip = 0; mip < mips; ++mip)
					desc.imageDimensions.push_back({ (std::max)(a_shape.width >> mip, 1u), (std::max)(a_shape.height >> mip, 1u), 0, 0 });
			return desc;
		}

		/** @brief A graph-only texture: aliasable (placed by the graph) when transient, materialized now otherwise. */
		std::shared_ptr<org::Resource> CreateGraphTexture(const Shape& a_shape, const std::string& a_name, bool a_aliasable,
			std::optional<std::uint64_t> a_pool = std::nullopt)
		{
			auto desc = Describe(a_shape, false);
			desc.allowAlias = a_aliasable;
			if (a_aliasable)
				desc.aliasingPoolID = a_pool;
			auto texture = a_aliasable ? org::PixelBuffer::CreateSharedUnmaterialized(desc) : org::PixelBuffer::CreateShared(desc);
			texture->SetName(a_name);
			return texture;
		}

		/** @brief An exported twin texture: every view in the texture's own format, the SRV always (Describe's rule). */
		Created CreateExportedTexture(const Shape& a_shape, const std::string& a_name)
		{
			const auto formats = FormatsOf(a_shape);
			ExportDesc desc{};
			desc.dimension = a_shape.dimension;
			desc.width = a_shape.width;
			desc.height = a_shape.height;
			desc.depthOrArraySize = a_shape.depthOrArraySize;
			desc.mips = a_shape.mips;
			desc.samples = a_shape.samples;
			desc.format = a_shape.format;
			desc.bindFlags = a_shape.bindFlags;
			desc.miscFlags = a_shape.miscFlags;
			desc.srv = formats.srv;
			desc.rtv = (a_shape.bindFlags & D3D11_BIND_RENDER_TARGET) ? a_shape.format : DXGI_FORMAT_UNKNOWN;
			desc.uav = (a_shape.bindFlags & D3D11_BIND_UNORDERED_ACCESS) ? a_shape.format : DXGI_FORMAT_UNKNOWN;
			desc.dsv = formats.dsv;
			auto exported = ExportTexture(desc, a_name);
			return { std::move(exported.graph), std::move(exported.d3d11) };
		}

		Created CreateTexture(const Shape& a_shape, const std::string& a_name, const Options& a_options)
		{
			if (!RenderGraphRuntime::Get().IsActive()) {
				logger::warn("[ORG] {}: not created, the render graph is not active", a_name);
				return {};
			}
			if (a_shape.usage != D3D11_USAGE_DEFAULT || a_shape.cpuAccess || (a_shape.miscFlags & ~UINT(D3D11_RESOURCE_MISC_TEXTURECUBE)) ||
				rhi::helpers::ToRHI(FormatsOf(a_shape).image) == rhi::Format::Unknown) {
				logger::error("[ORG] {}: not created: graph textures are DEFAULT usage, without CPU access, typed, and only cube misc flags", a_name);
				return {};
			}
			if (IsDepth(a_shape) && (Is3D(a_shape) || a_shape.depthOrArraySize > 1 || a_shape.samples > 1 || a_shape.miscFlags ||
										(a_shape.bindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS)))) {
				logger::error("[ORG] {}: not created: a graph depth texture is one 2D texture with one sample, without RTVs or UAVs", a_name);
				return {};
			}
			try {
				if (a_options.d3d11 == Export::None)
					return { CreateGraphTexture(a_shape, a_name, a_options.lifetime == Lifetime::Transient, a_options.aliasPool), nullptr };
				return CreateExportedTexture(a_shape, a_name);
			} catch (const std::exception& e) {
				logger::error("[ORG] {}: not created: {}", a_name, e.what());
				return {};
			}
		}

		/**
		 * @brief The exported texture's views of the whole texture; false when D3D11 refuses one. A depth texture's (a_depth: its
		 * Formats) are 2D views in its read and depth formats, which a typeless texture cannot default to. Only a depth texture
		 * exports a DSV.
		 */
		bool CreateTextureViews(ID3D11Resource* a_texture, Export a_export, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv,
			winrt::com_ptr<ID3D11UnorderedAccessView>& a_uav, winrt::com_ptr<ID3D11RenderTargetView>& a_rtv, winrt::com_ptr<ID3D11DepthStencilView>& a_dsv,
			const Formats* a_depth = nullptr)
		{
			auto* device = globals::d3d::device;
			if (a_depth) {
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.Format = a_depth->srv;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = static_cast<UINT>(-1);
				if (Has(a_export, Export::SRV) && FAILED(device->CreateShaderResourceView(a_texture, &srvDesc, a_srv.put())))
					return false;
				D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
				dsvDesc.Format = a_depth->dsv;
				dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
				return !Has(a_export, Export::DSV) || SUCCEEDED(device->CreateDepthStencilView(a_texture, &dsvDesc, a_dsv.put()));
			}
			if (Has(a_export, Export::DSV))
				return false;
			if (Has(a_export, Export::SRV) && FAILED(device->CreateShaderResourceView(a_texture, nullptr, a_srv.put())))
				return false;
			if (Has(a_export, Export::UAV) && FAILED(device->CreateUnorderedAccessView(a_texture, nullptr, a_uav.put())))
				return false;
			if (Has(a_export, Export::RTV) && FAILED(device->CreateRenderTargetView(a_texture, nullptr, a_rtv.put())))
				return false;
			return true;
		}

		/** @brief A graph buffer: aliasable (placed by the graph) when transient and not exported, materialized now otherwise. */
		void PlaceBuffer(org::Buffer& a_buffer, const std::string& a_name, bool a_aliasable)
		{
			a_buffer.SetName(a_name);
			if (a_aliasable)
				a_buffer.SetAllowAlias(true);
			else
				a_buffer.Materialize();
		}

		bool CheckBufferDesc(const D3D11_BUFFER_DESC& a_desc, const std::string& a_name)
		{
			if (!RenderGraphRuntime::Get().IsActive()) {
				logger::warn("[ORG] {}: not created, the render graph is not active", a_name);
				return false;
			}
			if (a_desc.Usage != D3D11_USAGE_DEFAULT || a_desc.CPUAccessFlags || !a_desc.ByteWidth) {
				logger::error("[ORG] {}: not created: graph buffers are DEFAULT usage, without CPU access, and not empty", a_name);
				return false;
			}
			return true;
		}

		/** @brief A 2D texture's mip count: 0 in the description is the whole chain, as in D3D11. */
		UINT MipsOf(const D3D11_TEXTURE2D_DESC& a_desc)
		{
			return a_desc.MipLevels ? a_desc.MipLevels : static_cast<UINT>(std::bit_width((std::max)({ a_desc.Width, a_desc.Height, 1u })));
		}
	}

	struct StagedUpload
	{
		struct Copy
		{
			std::uint32_t mip = 0, slice = 0, z = 0;
			rhi::CopyableFootprint footprint{};
		};
		std::shared_ptr<org::Resource> target;
		std::shared_ptr<org::Resource> staging;  // an upload-heap buffer, written once before any copy
		std::vector<Copy> copies;
	};

	namespace
	{
		/**
		 * @brief A texture's subresources in a new host-visible buffer, at the copy's pitches. Slice-major entries, as
		 * rhi::helpers::PlanTextureUploadSubresources reads them. Null, logged, without a texture or when the buffer cannot be made.
		 */
		std::shared_ptr<const StagedUpload> StageTexture(const std::shared_ptr<org::Resource>& a_texture, const std::string& a_name, DXGI_FORMAT a_format,
			UINT a_width, UINT a_height, UINT a_depth, UINT a_mips, UINT a_arraySize, std::span<const SubresourceData> a_subresources)
		{
			if (!a_texture || a_subresources.empty()) {
				logger::warn("[ORG] {}: not staged: {}", a_name, !a_texture ? "the texture was not created" : "no contents");
				return nullptr;
			}
			std::vector<rhi::helpers::SubresourceData> sources;
			sources.reserve(a_subresources.size());
			for (const auto& subresource : a_subresources)
				sources.push_back({ subresource.data, subresource.rowPitch, subresource.slicePitch });
			const rhi::Span<const rhi::helpers::SubresourceData> span{ sources.data(), static_cast<std::uint32_t>(sources.size()) };
			const auto plan = rhi::helpers::PlanTextureUploadSubresources(rhi::helpers::ToRHI(a_format), a_width, a_height, a_depth, a_mips, a_arraySize, span);
			if (!plan.totalSize || plan.footprints.empty()) {
				logger::error("[ORG] {}: not staged: no copy layout for format {}", a_name, static_cast<int>(a_format));
				return nullptr;
			}
			try {
				auto buffer = org::Buffer::CreateShared(rhi::HeapType::Upload, plan.totalSize);
				buffer->SetName(a_name + " Upload");
				auto resource = buffer->GetAPIResource();
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (!mapped) {
					logger::error("[ORG] {}: not staged: its upload buffer could not be mapped", a_name);
					return nullptr;
				}
				rhi::helpers::WriteTextureUploadSubresources(plan, span, mapped, 0);
				resource.Unmap(0, plan.totalSize);
				auto staged = std::make_shared<StagedUpload>();
				staged->target = a_texture;
				staged->staging = std::move(buffer);
				for (const auto& fp : plan.footprints)
					staged->copies.push_back({ fp.mip, fp.arraySlice, fp.zSlice, { fp.offset, fp.rowPitch, fp.width, fp.height, fp.depth } });
				return staged;
			} catch (const std::exception& e) {
				logger::error("[ORG] {}: not staged: {}", a_name, e.what());
				return nullptr;
			}
		}

		using StagedUploads = std::vector<std::shared_ptr<const StagedUpload>>;

		struct UploadBindings
		{
			std::vector<org::ResourceBindingToken> sources, targets;  // by upload
		};

		struct UploadFrame
		{
			std::shared_ptr<const StagedUploads> uploads;
		};

		/** @brief MakeUploadPass's pass: the staged copies, declared on both ends. */
		class UploadCopyPass final : public org::TypedRenderGraphPass<UploadCopyPass, UploadFrame, UploadBindings>
		{
		public:
			explicit UploadCopyPass(std::shared_ptr<const StagedUploads> a_uploads) :
				uploads(std::move(a_uploads)) {}

			UploadBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				UploadBindings bindings;
				for (const auto& upload : *uploads) {
					bindings.sources.push_back(a_builder.CopySource(upload->staging));
					bindings.targets.push_back(a_builder.CopyDestination(upload->target));
				}
				return bindings;
			}

			// Everything it records is fixed with its uploads.
			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(uploads.get())); }

			UploadFrame Prepare(const UploadBindings&, const org::PassPrepareContext&) const { return { uploads }; }

			static void Record(const UploadBindings& a_bindings, const UploadFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.uploads)
					return;
				auto& commands = a_recording.Commands();
				for (std::size_t i = 0; i < a_frame.uploads->size() && i < a_bindings.targets.size(); ++i) {
					const auto& upload = *(*a_frame.uploads)[i];
					const auto texture = a_recording.Resolve(a_bindings.targets[i]).GetHandle();
					const auto staging = a_recording.Resolve(a_bindings.sources[i]).GetHandle();
					for (const auto& copy : upload.copies) {
						rhi::BufferTextureCopyFootprint footprint{};
						footprint.texture = texture;
						footprint.buffer = staging;
						footprint.mip = copy.mip;
						footprint.arraySlice = copy.slice;
						footprint.z = copy.z;
						footprint.footprint = copy.footprint;
						commands.CopyBufferToTexture(footprint);
					}
				}
			}

		private:
			std::shared_ptr<const StagedUploads> uploads;
		};
	}

	std::shared_ptr<org::RenderPass> MakeUploadPass(std::vector<std::shared_ptr<const StagedUpload>> a_uploads)
	{
		std::erase(a_uploads, nullptr);
		if (a_uploads.empty())
			return nullptr;
		return std::make_shared<UploadCopyPass>(std::make_shared<const StagedUploads>(std::move(a_uploads)));
	}

	ExportedTexture ExportTexture(const ExportDesc& a_desc, const std::string& a_name)
	{
		auto& runtime = RenderGraphRuntime::Get();
		if (!runtime.CanWrapTextures()) {
			logger::warn("[ORG] {}: not exported, the DXVK build cannot wrap images (dxvkCreateTextureFromVkImage)", a_name);
			return {};
		}
		const bool is3D = a_desc.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
		const bool cube = (a_desc.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
		const UINT layers = (std::max)(a_desc.depthOrArraySize, 1u);
		const Shape shape{ a_desc.dimension, a_desc.width, a_desc.height, layers, a_desc.mips, a_desc.format, a_desc.samples, a_desc.bindFlags,
			a_desc.miscFlags, D3D11_USAGE_DEFAULT, 0 };
		const UINT mips = MipCount(shape);
		// A depth image is made in its DSV's format: a typeless texture format names no image format of its own.
		const auto imageFormat = rhi::helpers::ToRHI(a_desc.dsv != DXGI_FORMAT_UNKNOWN ? a_desc.dsv : a_desc.format);
		if (imageFormat == rhi::Format::Unknown) {
			logger::error("[ORG] {}: not exported, format {} has no image format", a_name, static_cast<int>(a_desc.format));
			return {};
		}
		try {
			rhi::ResourceDesc desc{};
			desc.type = is3D ? rhi::ResourceType::Texture3D : rhi::ResourceType::Texture2D;
			desc.heapType = rhi::HeapType::DeviceLocal;
			std::uint32_t flags = rhi::RF_None;
			if (a_desc.bindFlags & D3D11_BIND_RENDER_TARGET)
				flags |= rhi::RF_AllowRenderTarget;
			if (a_desc.bindFlags & D3D11_BIND_DEPTH_STENCIL)
				flags |= rhi::RF_AllowDepthStencil;
			if (a_desc.bindFlags & D3D11_BIND_UNORDERED_ACCESS)
				flags |= rhi::RF_AllowUnorderedAccess;
			if (cube)
				flags |= rhi::RF_TextureCubeCompatible;
			desc.resourceFlags = static_cast<rhi::ResourceFlags>(flags);
			desc.debugName = a_name.c_str();
			desc.texture.format = imageFormat;
			desc.texture.width = a_desc.width;
			desc.texture.height = a_desc.height;
			desc.texture.depthOrLayers = static_cast<std::uint16_t>(layers);
			desc.texture.mipLevels = static_cast<std::uint16_t>(mips);
			desc.texture.sampleCount = (std::max)(a_desc.samples, 1u);
			rhi::ResourcePtr image;
			if (runtime.Host()->Device().CreateCommittedResource(desc, image) != rhi::Result::Ok || !image) {
				logger::error("[ORG] {}: the image could not be created", a_name);
				return {};
			}
			DxvkOrgInteropTextureWrap wrap{};
			wrap.dimension = static_cast<std::uint32_t>(a_desc.dimension);
			wrap.width = a_desc.width;
			wrap.height = a_desc.height;
			wrap.depthOrArraySize = layers;
			wrap.mipLevels = mips;
			wrap.format = static_cast<std::uint32_t>(a_desc.format);
			wrap.sampleCount = (std::max)(a_desc.samples, 1u);
			wrap.bindFlags = a_desc.bindFlags;
			wrap.miscFlags = a_desc.miscFlags;
			DxvkOrgInteropResourceInfo info{};
			ExportedTexture exported;
			exported.d3d11 = runtime.WrapTexture(image.Get(), wrap, info, a_name.c_str());
			const auto home = exported.d3d11 ? RenderGraph::HomeLayoutOf(info.image.layout) : std::nullopt;
			if (!home) {
				if (exported.d3d11)
					logger::error("[ORG] {}: DXVK rests it in layout {}, which the graph has no equivalent of", a_name, static_cast<int>(info.image.layout));
				// DXVK's stream may still initialize the image: it goes once D3D11 is past this point.
				exported.d3d11 = nullptr;
				runtime.ReleaseAfterD3D11(std::make_shared<rhi::ResourcePtr>(std::move(image)));
				return {};
			}
			// The graph's description: the twins' (Describe) with each view in the format the owner asked for.
			auto graphDesc = Describe(shape, true);
			graphDesc.format = imageFormat;
			graphDesc.hasSRV = a_desc.srv != DXGI_FORMAT_UNKNOWN;
			graphDesc.srvFormat = rhi::helpers::ToRHI(a_desc.srv);
			graphDesc.hasUAV = a_desc.uav != DXGI_FORMAT_UNKNOWN;
			graphDesc.uavFormat = rhi::helpers::ToRHI(a_desc.uav);
			graphDesc.hasNonShaderVisibleUAV = graphDesc.hasUAV;
			graphDesc.hasRTV = a_desc.rtv != DXGI_FORMAT_UNKNOWN;
			graphDesc.rtvFormat = rhi::helpers::ToRHI(a_desc.rtv);
			graphDesc.hasDSV = a_desc.dsv != DXGI_FORMAT_UNKNOWN;
			graphDesc.dsvFormat = imageFormat;
			graphDesc.initialLayout = *home;
			std::shared_ptr<org::ExternalTextureResource> texture;
			if (*home == rhi::ResourceLayout::Common && !RenderGraph::Backend::IsD3D12()) {
				// Resting in GENERAL (native mode): the graph reads and renders it in GENERAL alone. That takes an import with
				// simultaneous access, since BasicRHI renders its own images in the attachment layouts. The import does not own the
				// image, so the graph texture keeps it: the import goes first, then the image.
				struct Kept
				{
					rhi::ResourcePtr image;
					std::shared_ptr<org::ExternalTextureResource> texture;
				};
				auto imported = RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), info.image, graphDesc, a_name.c_str());
				if (!imported) {
					logger::error("[ORG] {}: not exported, the graph could not import it in GENERAL", a_name);
					exported.d3d11 = nullptr;
					runtime.ReleaseAfterD3D11(std::make_shared<rhi::ResourcePtr>(std::move(image)));
					return {};
				}
				auto kept = std::make_shared<Kept>(Kept{ std::move(image), std::move(imported) });
				texture = std::shared_ptr<org::ExternalTextureResource>(kept, kept->texture.get());
			} else {
				// Common-only (every pass in GENERAL) is Vulkan's. D3D12's Common layout cannot be a render target, depth or UAV, so
				// there the graph moves the texture into each pass's layout and back.
				texture = org::ExternalTextureResource::CreateShared(std::move(image), graphDesc,
					*home == rhi::ResourceLayout::Common && !RenderGraph::Backend::IsD3D12());
				texture->SetName(a_name);
			}
			exported.graph = std::move(texture);
			exported.image = reinterpret_cast<std::uint64_t>(info.image.image);
			return exported;
		} catch (const std::exception& e) {
			logger::error("[ORG] {}: not exported: {}", a_name, e.what());
			return {};
		}
	}

	namespace
	{
		std::atomic<std::uint64_t> resizeGeneration{ 0 };
		std::atomic<std::uint64_t> structuralRebuilds{ 0 };
	}

	void RequestStructuralRebuild(std::string_view a_reason, std::span<const std::uint32_t> a_epochs)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		const auto count = structuralRebuilds.fetch_add(1, std::memory_order_relaxed) + 1;
		const auto reason = fmt::format("structural: {}", a_reason);
		if (a_epochs.empty()) {
			logger::info("[ORG] structural rebuild {} (every epoch): {}", count, a_reason);
			host->RequestRebuild(reason);
			return;
		}
		logger::info("[ORG] structural rebuild {} ({} epochs): {}", count, a_epochs.size(), a_reason);
		for (const auto epoch : a_epochs)
			host->RequestRebuild(epoch, reason);
	}

	std::uint64_t StructuralRebuilds()
	{
		return structuralRebuilds.load(std::memory_order_relaxed);
	}

	bool Resource::AdoptSlot(std::shared_ptr<org::VersionedTexture> a_previous, bool a_sameShape)
	{
		slot.reset();
		auto image = std::dynamic_pointer_cast<org::GloballyIndexedResource>(graph);
		if (!options.versioned || Aliasable() || !image)
			return false;
		try {
			if (a_previous && a_sameShape) {
				a_previous->PublishImage(std::move(image));
				slot = std::move(a_previous);
				// Tickets prepared before resolved the old version: they are prepared again.
				if (auto* host = RenderGraphRuntime::Get().Host())
					host->NoteNewVersions();
				return true;
			}
			slot = org::VersionedTexture::CreateImage(std::move(image));
		} catch (const std::exception& e) {
			// Another shape class than the first version: a new slot, which the passes declare again after a build.
			logger::warn("[ORG] {}: not a new version ({}); a new slot instead", name, e.what());
			slot = org::VersionedTexture::CreateImage(std::dynamic_pointer_cast<org::GloballyIndexedResource>(graph));
		}
		if (a_previous)
			logger::info("[ORG] {}: another shape class after a resize; a new slot, built again", name);
		return false;
	}

	std::uint64_t Resource::ResizeGeneration()
	{
		return resizeGeneration.load(std::memory_order_acquire);
	}

	void Resource::BumpResizeGeneration()
	{
		resizeGeneration.fetch_add(1, std::memory_order_acq_rel);
	}

	void Resource::Register(org::RenderGraph& a_graph) const
	{
		if (slot) {
			// PublishedResources may have registered the same slot under the same id already: a graph takes one resolver an id.
			const org::ResourceIdentifier id(name);
			if (!a_graph._resolverMap.contains(id))
				a_graph.RegisterResolver(id, slot);
			return;
		}
		if (graph)
			a_graph.RegisterResource(org::ResourceIdentifier(name), graph);
	}

	void Resource::Reset()
	{
		// The D3D11 side first: nothing of D3D11 may outlive the memory it views.
		srv = nullptr;
		uav = nullptr;
		rtv = nullptr;
		dsv = nullptr;
		const bool exported = d3d11 != nullptr;
		d3d11 = nullptr;
		if (exported && graph)
			RenderGraphRuntime::Get().ReleaseAfterD3D11(std::move(graph));
		graph.reset();
		// The slot's versions keep their textures while a frame or a recording holds them.
		slot.reset();
	}

	void Resource::NameViews() const
	{
		if (srv)
			Util::SetResourceName(srv.get(), "%s SRV", name.c_str());
		if (uav)
			Util::SetResourceName(uav.get(), "%s UAV", name.c_str());
		if (rtv)
			Util::SetResourceName(rtv.get(), "%s RTV", name.c_str());
		if (dsv)
			Util::SetResourceName(dsv.get(), "%s DSV", name.c_str());
	}

	Texture2D::Texture2D(const D3D11_TEXTURE2D_DESC& a_desc, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options), desc(a_desc)
	{
		Create();
		AdoptSlot(nullptr, false);
	}

	void Texture2D::Create()
	{
		const Shape shape{ D3D11_RESOURCE_DIMENSION_TEXTURE2D, desc.Width, desc.Height, desc.ArraySize, desc.MipLevels, desc.Format,
			desc.SampleDesc.Count, desc.BindFlags, desc.MiscFlags, desc.Usage, desc.CPUAccessFlags };
		auto created = CreateTexture(shape, name, options);
		graph = std::move(created.graph);
		d3d11 = std::move(created.d3d11);
		const auto formats = FormatsOf(shape);
		const Formats* depth = IsDepth(shape) ? &formats : nullptr;
		if (d3d11 && (!d3d11.try_as<ID3D11Texture2D>() || !CreateTextureViews(d3d11.get(), options.d3d11, srv, uav, rtv, dsv, depth))) {
			logger::error("[ORG] {}: D3D11 refused a view of the exported texture", name);
			Reset();
			return;
		}
		NameViews();
	}

	std::shared_ptr<const StagedUpload> Texture2D::Stage(std::span<const SubresourceData> a_subresources, UINT a_width, UINT a_height) const
	{
		const UINT width = a_width ? (std::min)(a_width, desc.Width) : desc.Width;
		const UINT height = a_height ? (std::min)(a_height, desc.Height) : desc.Height;
		return StageTexture(graph, name, desc.Format, width, height, 1, MipsOf(desc), desc.ArraySize, a_subresources);
	}

	std::shared_ptr<const StagedUpload> Texture2D::StageZeros() const
	{
		const auto format = rhi::helpers::ToRHI(desc.Format);
		const bool blocks = rhi::helpers::IsBlockCompressed(format);
		const UINT bytes = rhi::helpers::BytesPerBlock(format);
		// One buffer of zeros as large as mip 0: every subresource reads it with its own row pitch.
		const auto rowBytes = [&](UINT a_mip) { const UINT width = (std::max)(desc.Width >> a_mip, 1u); return (blocks ? (width + 3) / 4 : width) * bytes; };
		const auto rows = [&](UINT a_mip) { const UINT height = (std::max)(desc.Height >> a_mip, 1u); return blocks ? (height + 3) / 4 : height; };
		const std::vector<std::uint8_t> zeros(static_cast<std::size_t>(rowBytes(0)) * rows(0), 0);
		std::vector<SubresourceData> subresources;
		for (UINT slice = 0; slice < desc.ArraySize; ++slice)
			for (UINT mip = 0; mip < MipsOf(desc); ++mip)
				subresources.push_back({ zeros.data(), rowBytes(mip), rowBytes(mip) * rows(mip) });
		return Stage(subresources);
	}

	bool Texture2D::Resize(UINT a_width, UINT a_height)
	{
		if (a_width == desc.Width && a_height == desc.Height)
			return false;
		const UINT mips = MipsOf(desc);
		auto previous = slot;
		Reset();
		desc.Width = a_width;
		desc.Height = a_height;
		Create();
		if (AdoptSlot(std::move(previous), MipsOf(desc) == mips)) {
			logger::info("[ORG] {}: resized to {}x{} as a new version", name, a_width, a_height);
			return true;
		}
		BumpResizeGeneration();
		return true;
	}

	Texture3D::Texture3D(const D3D11_TEXTURE3D_DESC& a_desc, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options), desc(a_desc)
	{
		Create();
		AdoptSlot(nullptr, false);
	}

	void Texture3D::Create()
	{
		const Shape shape{ D3D11_RESOURCE_DIMENSION_TEXTURE3D, desc.Width, desc.Height, desc.Depth, desc.MipLevels, desc.Format, 1u,
			desc.BindFlags, desc.MiscFlags, desc.Usage, desc.CPUAccessFlags };
		if (Has(options.d3d11, Export::RTV) || (desc.BindFlags & D3D11_BIND_RENDER_TARGET)) {
			logger::error("[ORG] {}: not created: graph 3D textures take no render targets", name);
			return;
		}
		auto created = CreateTexture(shape, name, options);
		graph = std::move(created.graph);
		d3d11 = std::move(created.d3d11);
		if (d3d11 && (!d3d11.try_as<ID3D11Texture3D>() || !CreateTextureViews(d3d11.get(), options.d3d11, srv, uav, rtv, dsv))) {
			logger::error("[ORG] {}: D3D11 refused a view of the exported texture", name);
			Reset();
			return;
		}
		NameViews();
	}

	std::shared_ptr<const StagedUpload> Texture3D::Stage(std::span<const SubresourceData> a_depthSlices) const
	{
		return StageTexture(graph, name, desc.Format, desc.Width, desc.Height, desc.Depth, 1, 1, a_depthSlices);
	}

	bool Texture3D::Resize(UINT a_width, UINT a_height, UINT a_depth)
	{
		if (a_width == desc.Width && a_height == desc.Height && a_depth == desc.Depth)
			return false;
		const auto mipsOf = [](const D3D11_TEXTURE3D_DESC& a_desc) {
			return a_desc.MipLevels ? a_desc.MipLevels : static_cast<UINT>(std::bit_width((std::max)({ a_desc.Width, a_desc.Height, a_desc.Depth, 1u })));
		};
		const UINT mips = mipsOf(desc);
		auto previous = slot;
		Reset();
		desc.Width = a_width;
		desc.Height = a_height;
		desc.Depth = a_depth;
		Create();
		if (AdoptSlot(std::move(previous), mipsOf(desc) == mips)) {
			logger::info("[ORG] {}: resized to {}x{}x{} as a new version", name, a_width, a_height, a_depth);
			return true;
		}
		BumpResizeGeneration();
		return true;
	}

	StructuredBuffer::StructuredBuffer(const D3D11_BUFFER_DESC& a_desc, UINT a_count, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options), count(a_count)
	{
		if (!CheckBufferDesc(a_desc, name) || !a_count || !a_desc.StructureByteStride || Has(options.d3d11, Export::RTV))
			return;
		const bool unordered = (a_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
		try {
			auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(a_count, a_desc.StructureByteStride, unordered);
			PlaceBuffer(*buffer, name, Aliasable());
			if (Exported()) {
				D3D11_BUFFER_DESC wrapDesc{};
				wrapDesc.ByteWidth = a_count * a_desc.StructureByteStride;
				wrapDesc.Usage = D3D11_USAGE_DEFAULT;
				wrapDesc.BindFlags = a_desc.BindFlags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
				wrapDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				wrapDesc.StructureByteStride = a_desc.StructureByteStride;
				auto wrapped = RenderGraphRuntime::Get().WrapBuffer(*buffer, wrapDesc);
				d3d11.copy_from(wrapped.get());
				graph = std::move(buffer);
				if (!wrapped) {
					Reset();
					return;
				}
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.Format = DXGI_FORMAT_UNKNOWN;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				srvDesc.Buffer.NumElements = a_count;
				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				uavDesc.Format = DXGI_FORMAT_UNKNOWN;
				uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
				uavDesc.Buffer.NumElements = a_count;
				auto* device = globals::d3d::device;
				if ((Has(options.d3d11, Export::SRV) && FAILED(device->CreateShaderResourceView(wrapped.get(), &srvDesc, srv.put()))) ||
					(Has(options.d3d11, Export::UAV) && FAILED(device->CreateUnorderedAccessView(wrapped.get(), &uavDesc, uav.put())))) {
					logger::error("[ORG] {}: D3D11 refused a view of the exported buffer", name);
					Reset();
					return;
				}
				NameViews();
				return;
			}
			graph = std::move(buffer);
		} catch (const std::exception& e) {
			logger::error("[ORG] {}: not created: {}", name, e.what());
			Reset();
		}
	}

	Buffer::Buffer(const D3D11_BUFFER_DESC& a_desc, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options)
	{
		if (!CheckBufferDesc(a_desc, name) || Has(options.d3d11, Export::RTV))
			return;
		const bool unordered = (a_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
		try {
			auto buffer = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, a_desc.ByteWidth, unordered);
			PlaceBuffer(*buffer, name, Aliasable());
			if (Exported()) {
				D3D11_BUFFER_DESC wrapDesc = a_desc;
				wrapDesc.BindFlags &= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				d3d11.copy_from(RenderGraphRuntime::Get().WrapBuffer(*buffer, wrapDesc).get());
				graph = std::move(buffer);
				if (!d3d11)
					Reset();
				return;
			}
			graph = std::move(buffer);
		} catch (const std::exception& e) {
			logger::error("[ORG] {}: not created: {}", name, e.what());
			Reset();
		}
	}

	bool Buffer::CreateSRV(const D3D11_SHADER_RESOURCE_VIEW_DESC& a_desc)
	{
		srv = nullptr;
		if (!d3d11 || FAILED(globals::d3d::device->CreateShaderResourceView(d3d11.get(), &a_desc, srv.put())))
			return false;
		NameViews();
		return true;
	}

	bool Buffer::CreateUAV(const D3D11_UNORDERED_ACCESS_VIEW_DESC& a_desc)
	{
		uav = nullptr;
		if (!d3d11 || FAILED(globals::d3d::device->CreateUnorderedAccessView(d3d11.get(), &a_desc, uav.put())))
			return false;
		NameViews();
		return true;
	}
}

#else  // !CS_HAS_RENDER_GRAPH

#	include "RenderGraph/GraphResources.h"

// Built without OpenRenderGraph: no twin is ever created, and every feature keeps its D3D11 path.
namespace GraphResources
{
	ExportedTexture ExportTexture(const ExportDesc&, const std::string&) { return {}; }
	void RequestStructuralRebuild(std::string_view, std::span<const std::uint32_t>) {}
	std::uint64_t StructuralRebuilds() { return 0; }
	bool Resource::AdoptSlot(std::shared_ptr<org::VersionedTexture>, bool) { return false; }
	std::uint64_t Resource::ResizeGeneration() { return 0; }
	void Resource::BumpResizeGeneration() {}
	void Resource::Register(org::RenderGraph&) const {}
	void Resource::Reset() {}
	void Resource::NameViews() const {}
	Texture2D::Texture2D(const D3D11_TEXTURE2D_DESC& a_desc, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options), desc(a_desc) {}
	void Texture2D::Create() {}
	struct StagedUpload
	{};
	std::shared_ptr<org::RenderPass> MakeUploadPass(std::vector<std::shared_ptr<const StagedUpload>>) { return nullptr; }
	std::shared_ptr<const StagedUpload> Texture2D::Stage(std::span<const SubresourceData>, UINT, UINT) const { return nullptr; }
	std::shared_ptr<const StagedUpload> Texture2D::StageZeros() const { return nullptr; }
	bool Texture2D::Resize(UINT, UINT) { return false; }
	Texture3D::Texture3D(const D3D11_TEXTURE3D_DESC& a_desc, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options), desc(a_desc) {}
	void Texture3D::Create() {}
	std::shared_ptr<const StagedUpload> Texture3D::Stage(std::span<const SubresourceData>) const { return nullptr; }
	bool Texture3D::Resize(UINT, UINT, UINT) { return false; }
	StructuredBuffer::StructuredBuffer(const D3D11_BUFFER_DESC&, UINT a_count, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options), count(a_count) {}
	Buffer::Buffer(const D3D11_BUFFER_DESC&, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options) {}
	bool Buffer::CreateSRV(const D3D11_SHADER_RESOURCE_VIEW_DESC&) { return false; }
	bool Buffer::CreateUAV(const D3D11_UNORDERED_ACCESS_VIEW_DESC&) { return false; }
	namespace SelfTest
	{
		bool Requested() { return false; }
		void Reserve() {}
		void Setup() {}
	}
}

#endif

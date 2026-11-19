#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/GraphResources.h"

#	include "Globals.h"
#	include "RenderGraph/Backend.h"
#	include "RenderGraph/BuildManager.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/Frame/FrameDriver.h"
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
#	include <deque>
#	include <map>
#	include <mutex>
#	include <shared_mutex>
#	include <vector>

namespace GraphResources
{
	namespace
	{
		struct Shape
		{
			D3D11_RESOURCE_DIMENSION dimension;
			UINT width, height;
			UINT depthOrArraySize;  // depth of a 3D texture, array size of a 2D one
			UINT mips;              // 0 means the whole chain, as in D3D11
			DXGI_FORMAT format;
			UINT samples;
			UINT bindFlags;
			UINT miscFlags;
			D3D11_USAGE usage;
			UINT cpuAccess;
			bool array = false;  // array views even with one slice
		};

		struct Created
		{
			std::shared_ptr<org::Resource> graph;
			winrt::com_ptr<ID3D11Resource> d3d11;
		};

		bool Is3D(const Shape& a_shape) { return a_shape.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D; }

		bool IsDepth(const Shape& a_shape) { return (a_shape.bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0; }

		/** @brief A texture's image and view formats. A typeless depth texture gets its view formats from Texture2D's rule. */
		struct Formats
		{
			DXGI_FORMAT image = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT srv = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;  // depth textures only
		};

		/** @brief All unknown for a depth texture whose format is not a typeless depth format. */
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

		/** @brief A PixelBuffer reads mips from one entry per slice and mip, an external texture from one entry per mip. */
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
			desc.isArray = a_shape.array || slices > (cube ? 6u : 1u);
			desc.depth = Is3D(a_shape) ? a_shape.depthOrArraySize : 1u;
			desc.sampleCount = static_cast<std::uint8_t>((std::max)(a_shape.samples, 1u));
			desc.hasSRV = true;
			desc.srvFormat = rhi::helpers::ToRHI(formats.srv);
			desc.hasUAV = (a_shape.bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
			desc.uavFormat = format;
			// A CPU-side UAV too, so a pass can clear it as D3D11 clears its UAVs.
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

		/** @brief A graph-only texture. Transient ones are placed by the graph, others are made now. */
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

		/** @brief An exported twin texture with every view in its own format. */
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
			desc.array = a_shape.array;
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
		 * @brief The exported texture's whole-texture views, false when D3D11 refuses one.
		 * Depth views are 2D views in explicit formats, since a typeless texture cannot default to them.
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

		/** @brief A graph buffer. Transient and unexported ones are placed by the graph, others are made now. */
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

		/** @brief 0 in the description means the whole chain, as in D3D11. */
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
		std::shared_ptr<org::Resource> staging;  // upload heap, written once before any copy
		std::vector<Copy> copies;
	};

	namespace
	{
		/** @brief Copies a texture's subresources into a new host-visible buffer, slice-major. Null and logged on failure. */
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
			a_desc.miscFlags, D3D11_USAGE_DEFAULT, 0, a_desc.array };
		const UINT mips = MipCount(shape);
		// A typeless format names no image format, so a depth image uses its DSV's format.
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
				// DXVK's stream may still initialize the image, so it goes once D3D11 is past this point.
				exported.d3d11 = nullptr;
				runtime.ReleaseAfterD3D11(std::make_shared<rhi::ResourcePtr>(std::move(image)));
				return {};
			}
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
			if (*home == rhi::ResourceLayout::Common) {
				// Native mode keeps the image in Common and the graph moves it between layouts. Other users go through this resource.
				// The import does not own the image, so the import is released first and then the image.
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
				// Guest mode rests the texture in its own layout and the graph moves it into each pass's layout and back.
				texture = org::ExternalTextureResource::CreateShared(std::move(image), graphDesc, false);
				texture->SetName(a_name);
				// For the duplicate-image check.
				RenderGraph::NoteImageResource(reinterpret_cast<std::uint64_t>(info.image.image), texture, a_name);
			}
			exported.image = reinterpret_cast<std::uint64_t>(info.image.image);
			// An import of the D3D11 texture finds this resource instead of importing it again.
			RenderGraph::RegisterMadeImage(exported.image, texture, graphDesc, a_name);
			exported.graph = std::move(texture);
			return exported;
		} catch (const std::exception& e) {
			logger::error("[ORG] {}: not exported: {}", a_name, e.what());
			return {};
		}
	}

	namespace
	{
		std::atomic<std::uint64_t> resizeGeneration{ 0 };

		// The last resize generation bumps with their causes, named when the anchors build again.
		constexpr std::size_t kResizeCauses = 16;
		std::mutex resizeCauseLock;
		std::deque<std::pair<std::uint64_t, std::string>> resizeCauses;
		std::atomic<std::uint64_t> structuralRebuilds{ 0 };

		/** @brief Register may run on the build thread while Resize replaces a twin on the render thread. Register holds it shared. */
		std::shared_mutex& RegisterMutex()
		{
			static std::shared_mutex s_mutex;
			return s_mutex;
		}
	}

	void RequestStructuralRebuild(std::string_view a_reason, std::span<const std::uint32_t> a_epochs)
	{
		// The build manager builds this off the render thread and attaches it at a frame boundary.
		if (!RenderGraphRuntime::Get().Host())
			return;
		const auto count = structuralRebuilds.fetch_add(1, std::memory_order_relaxed) + 1;
		const auto reason = fmt::format("structural: {}", a_reason);
		if (a_epochs.empty()) {
			logger::info("[ORG] structural rebuild {} (every epoch): {}", count, a_reason);
			RenderGraph::Builds::RequestAll(reason);
			return;
		}
		logger::info("[ORG] structural rebuild {} ({} epochs): {}", count, a_epochs.size(), a_reason);
		for (const auto epoch : a_epochs)
			RenderGraph::Builds::Request(epoch, reason);
	}

	std::uint64_t StructuralRebuilds()
	{
		return structuralRebuilds.load(std::memory_order_relaxed);
	}

	namespace
	{
		/** @brief The live transient twins' names, counted, since two twins may share a name across a resize. */
		struct TransientNames
		{
			std::mutex mutex;
			std::map<std::string, std::uint32_t, std::less<>> names;
		};

		TransientNames& Transients()
		{
			// Never destroyed, since twins are destroyed during static destruction too.
			static auto* names = new TransientNames;
			return *names;
		}
	}

	void NoteTransientName(const std::string& a_name, bool a_live)
	{
		auto& transients = Transients();
		const std::lock_guard lock(transients.mutex);
		if (a_live)
			++transients.names[a_name];
		else if (const auto found = transients.names.find(a_name); found != transients.names.end() && --found->second == 0)
			transients.names.erase(found);
	}

	bool IsTransientName(std::string_view a_name)
	{
		auto& transients = Transients();
		const std::lock_guard lock(transients.mutex);
		return transients.names.contains(a_name);
	}

	bool Resource::AdoptSlot(std::shared_ptr<org::VersionedTexture> a_previous, bool a_sameShape)
	{
		const bool joined = JoinSlot(std::move(a_previous), a_sameShape);
		// An import of the export returns the slot, which passes follow across versions.
		if (slot && graph)
			RenderGraph::SetMadeImageSlot(graph.get(), slot);
		return joined;
	}

	bool Resource::JoinSlot(std::shared_ptr<org::VersionedTexture> a_previous, bool a_sameShape)
	{
		slot.reset();
		auto image = std::dynamic_pointer_cast<org::GloballyIndexedResource>(graph);
		if (!options.versioned || Aliasable() || !image)
			return false;
		try {
			if (a_previous && a_sameShape) {
				a_previous->PublishImage(std::move(image));
				slot = std::move(a_previous);
				// Tickets prepared before resolved the old version, so they are prepared again.
				if (auto* host = RenderGraphRuntime::Get().Host())
					host->NoteNewVersions();
				return true;
			}
			slot = org::VersionedTexture::CreateImage(std::move(image));
		} catch (const std::exception& e) {
			// A different shape class needs a new slot, which passes declare again after a build.
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

	void Resource::BumpResizeGeneration(std::string_view a_who)
	{
		std::scoped_lock lock(resizeCauseLock);
		const auto generation = resizeGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
		resizeCauses.emplace_back(generation, std::string(a_who));
		if (resizeCauses.size() > kResizeCauses)
			resizeCauses.pop_front();
	}

	std::string Resource::ResizeCauses(std::uint64_t a_since)
	{
		std::scoped_lock lock(resizeCauseLock);
		std::string causes;
		for (const auto& [generation, who] : resizeCauses) {
			if (generation <= a_since)
				continue;
			if (!causes.empty())
				causes += ", ";
			causes += who;
		}
		return causes.empty() ? std::string("unknown") : causes;
	}

	void Resource::Register(org::RenderGraph& a_graph) const
	{
		std::shared_lock lock(RegisterMutex());
		if (slot) {
			// PublishedResources may have registered the same id already, and a graph takes one resolver per id.
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
		// Release the D3D11 side first, since nothing of D3D11 may outlive the memory it views.
		srv = nullptr;
		uav = nullptr;
		rtv = nullptr;
		dsv = nullptr;
		const bool exported = d3d11 != nullptr;
		d3d11 = nullptr;
		RenderGraph::ForgetMadeImage(graph.get());
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
			desc.SampleDesc.Count, desc.BindFlags, desc.MiscFlags, desc.Usage, desc.CPUAccessFlags, options.array };
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
		// One zero buffer as large as mip 0. Every subresource reads it with its own row pitch.
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
		std::unique_lock lock(RegisterMutex());
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
		BumpResizeGeneration(name);
		return true;
	}

	namespace
	{
		struct SizeRequest
		{
			std::weak_ptr<const void> alive;
			std::function<void()> apply;
		};

		std::vector<SizeRequest>& SizeRequests()
		{
			static std::vector<SizeRequest> s_requests;
			return s_requests;
		}

		/** @brief Applies every waiting resize at the frame boundary. A twin destroyed since has none. */
		void ApplySizeRequests()
		{
			for (auto& request : std::exchange(SizeRequests(), {}))
				if (const auto alive = request.alive.lock())
					request.apply();
		}
	}

	void Resource::RequestExtent(const std::array<UINT, 3>& a_size, bool a_current, std::function<void(const std::array<UINT, 3>&)> a_resize)
	{
		if (a_current) {
			requestedSize.reset();
			return;
		}
		if (requestedSize == a_size)
			return;
		const bool queued = requestedSize.has_value();
		requestedSize = a_size;
		if (queued)
			return;
		auto apply = [this, resize = std::move(a_resize)] {
			const auto size = std::exchange(requestedSize, std::nullopt);
			if (!size)
				return;
			resize(*size);
			if (!graph || (Exported() && !d3d11)) {
				const auto message = fmt::format("Community Shaders: {} could not be made again at {}x{}x{} at a frame boundary", name, (*size)[0], (*size)[1], (*size)[2]);
				logger::critical("[ORG] {}", message);
				stl::report_and_fail(message);
			}
		};
		auto& requests = SizeRequests();
		requests.push_back({ alive, std::move(apply) });
		if (requests.size() == 1)
			RenderGraph::Frame::Driver::AtNextBegin(ApplySizeRequests);
	}

	void Texture2D::RequestSize(UINT a_width, UINT a_height)
	{
		RequestExtent({ a_width, a_height, 1 }, a_width == desc.Width && a_height == desc.Height,
			[this](const std::array<UINT, 3>& a_size) { Resize(a_size[0], a_size[1]); });
	}

	Texture3D::Texture3D(const D3D11_TEXTURE3D_DESC& a_desc, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options), desc(a_desc)
	{
		Create();
		AdoptSlot(nullptr, false);
	}

	void Texture3D::RequestSize(UINT a_width, UINT a_height, UINT a_depth)
	{
		RequestExtent({ a_width, a_height, a_depth }, a_width == desc.Width && a_height == desc.Height && a_depth == desc.Depth,
			[this](const std::array<UINT, 3>& a_size) { Resize(a_size[0], a_size[1], a_size[2]); });
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
		std::unique_lock lock(RegisterMutex());
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
		BumpResizeGeneration(name);
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

// Without OpenRenderGraph no twin is created and every feature keeps its D3D11 path.
namespace GraphResources
{
	ExportedTexture ExportTexture(const ExportDesc&, const std::string&) { return {}; }
	void RequestStructuralRebuild(std::string_view, std::span<const std::uint32_t>) {}
	std::uint64_t StructuralRebuilds() { return 0; }
	bool IsTransientName(std::string_view) { return false; }
	void NoteTransientName(const std::string&, bool) {}
	bool Resource::AdoptSlot(std::shared_ptr<org::VersionedTexture>, bool) { return false; }
	std::uint64_t Resource::ResizeGeneration() { return 0; }
	std::string Resource::ResizeCauses(std::uint64_t) { return {}; }
	void Resource::BumpResizeGeneration(std::string_view) {}
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
	void Texture2D::RequestSize(UINT, UINT) {}
	Texture3D::Texture3D(const D3D11_TEXTURE3D_DESC& a_desc, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options), desc(a_desc) {}
	void Texture3D::Create() {}
	std::shared_ptr<const StagedUpload> Texture3D::Stage(std::span<const SubresourceData>) const { return nullptr; }
	bool Texture3D::Resize(UINT, UINT, UINT) { return false; }
	void Texture3D::RequestSize(UINT, UINT, UINT) {}
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

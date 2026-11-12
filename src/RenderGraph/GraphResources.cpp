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
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <Resources/TextureDescription.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <atomic>
#	include <bit>

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
			const auto format = rhi::helpers::ToRHI(a_shape.format);
			org::TextureDescription desc{};
			desc.type = Is3D(a_shape) ? rhi::ResourceType::Texture3D : rhi::ResourceType::Texture2D;
			desc.format = format;
			desc.isCubemap = cube;
			desc.arraySize = cube ? slices / 6 : slices;
			desc.isArray = slices > (cube ? 6u : 1u);
			desc.depth = Is3D(a_shape) ? a_shape.depthOrArraySize : 1u;
			desc.sampleCount = static_cast<std::uint8_t>((std::max)(a_shape.samples, 1u));
			desc.hasSRV = true;
			desc.srvFormat = format;
			desc.hasUAV = (a_shape.bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
			desc.uavFormat = format;
			// A CPU-side UAV too, so a pass can clear it (PassBuilder::UnorderedAccessClear), as D3D11 clears its UAVs.
			desc.hasNonShaderVisibleUAV = desc.hasUAV;
			desc.hasRTV = (a_shape.bindFlags & D3D11_BIND_RENDER_TARGET) != 0;
			desc.rtvFormat = format;
			desc.hasDSV = (a_shape.bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
			desc.dsvFormat = format;
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
			desc.srv = a_shape.format;
			desc.rtv = (a_shape.bindFlags & D3D11_BIND_RENDER_TARGET) ? a_shape.format : DXGI_FORMAT_UNKNOWN;
			desc.uav = (a_shape.bindFlags & D3D11_BIND_UNORDERED_ACCESS) ? a_shape.format : DXGI_FORMAT_UNKNOWN;
			desc.dsv = (a_shape.bindFlags & D3D11_BIND_DEPTH_STENCIL) ? a_shape.format : DXGI_FORMAT_UNKNOWN;
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
				rhi::helpers::ToRHI(a_shape.format) == rhi::Format::Unknown) {
				logger::error("[ORG] {}: not created: graph textures are DEFAULT usage, without CPU access, typed, and only cube misc flags", a_name);
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

		/** @brief The exported texture's views of the whole texture; false when D3D11 refuses one. */
		bool CreateTextureViews(ID3D11Resource* a_texture, Export a_export, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv,
			winrt::com_ptr<ID3D11UnorderedAccessView>& a_uav, winrt::com_ptr<ID3D11RenderTargetView>& a_rtv)
		{
			auto* device = globals::d3d::device;
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
				texture = org::ExternalTextureResource::CreateShared(std::move(image), graphDesc, *home == rhi::ResourceLayout::Common);
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
		if (graph)
			a_graph.RegisterResource(org::ResourceIdentifier(name), graph);
	}

	void Resource::Reset()
	{
		// The D3D11 side first: nothing of D3D11 may outlive the memory it views.
		srv = nullptr;
		uav = nullptr;
		rtv = nullptr;
		const bool exported = d3d11 != nullptr;
		d3d11 = nullptr;
		if (exported && graph)
			RenderGraphRuntime::Get().ReleaseAfterD3D11(std::move(graph));
		graph.reset();
	}

	void Resource::NameViews() const
	{
		if (srv)
			Util::SetResourceName(srv.get(), "%s SRV", name.c_str());
		if (uav)
			Util::SetResourceName(uav.get(), "%s UAV", name.c_str());
		if (rtv)
			Util::SetResourceName(rtv.get(), "%s RTV", name.c_str());
	}

	Texture2D::Texture2D(const D3D11_TEXTURE2D_DESC& a_desc, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options), desc(a_desc)
	{
		Create();
	}

	void Texture2D::Create()
	{
		const Shape shape{ D3D11_RESOURCE_DIMENSION_TEXTURE2D, desc.Width, desc.Height, desc.ArraySize, desc.MipLevels, desc.Format,
			desc.SampleDesc.Count, desc.BindFlags, desc.MiscFlags, desc.Usage, desc.CPUAccessFlags };
		auto created = CreateTexture(shape, name, options);
		graph = std::move(created.graph);
		d3d11 = std::move(created.d3d11);
		if (d3d11 && (!d3d11.try_as<ID3D11Texture2D>() || !CreateTextureViews(d3d11.get(), options.d3d11, srv, uav, rtv))) {
			logger::error("[ORG] {}: D3D11 refused a view of the exported texture", name);
			Reset();
			return;
		}
		NameViews();
	}

	bool Texture2D::Resize(UINT a_width, UINT a_height)
	{
		if (a_width == desc.Width && a_height == desc.Height)
			return false;
		Reset();
		desc.Width = a_width;
		desc.Height = a_height;
		Create();
		BumpResizeGeneration();
		return true;
	}

	Texture3D::Texture3D(const D3D11_TEXTURE3D_DESC& a_desc, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options), desc(a_desc)
	{
		Create();
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
		if (d3d11 && (!d3d11.try_as<ID3D11Texture3D>() || !CreateTextureViews(d3d11.get(), options.d3d11, srv, uav, rtv))) {
			logger::error("[ORG] {}: D3D11 refused a view of the exported texture", name);
			Reset();
			return;
		}
		NameViews();
	}

	bool Texture3D::Resize(UINT a_width, UINT a_height, UINT a_depth)
	{
		if (a_width == desc.Width && a_height == desc.Height && a_depth == desc.Depth)
			return false;
		Reset();
		desc.Width = a_width;
		desc.Height = a_height;
		desc.Depth = a_depth;
		Create();
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
	std::uint64_t Resource::ResizeGeneration() { return 0; }
	void Resource::BumpResizeGeneration() {}
	void Resource::Register(org::RenderGraph&) const {}
	void Resource::Reset() {}
	void Resource::NameViews() const {}
	Texture2D::Texture2D(const D3D11_TEXTURE2D_DESC& a_desc, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options), desc(a_desc) {}
	void Texture2D::Create() {}
	bool Texture2D::Resize(UINT, UINT) { return false; }
	Texture3D::Texture3D(const D3D11_TEXTURE3D_DESC& a_desc, std::string a_name, Options a_options) :
		Resource(std::move(a_name), a_options), desc(a_desc) {}
	void Texture3D::Create() {}
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

#include "RenderGraph/Engine/Textures/EngineTextures.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Globals.h"
#include "RenderGraph/DxvkImageImport.h"
#include "RenderGraph/ImageRegistry.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/Engine/Tripwire/GuestDevice.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/UndeclaredAccess.h"
#include "Utils/D3D.h"

#include <OpenRenderGraph/GraphHost.h>
#include <OpenRenderGraph/Runtime.h>
#include <Render/Runtime/IUploadService.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/PixelBuffer.h>
#include <Resources/TextureDescription.h>
#include <rhi_helpers.h>

#include <DirectXTex.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace RenderGraph::Engine::Textures
{
	namespace
	{
		std::atomic<std::uint64_t> g_created{ 0 }, g_live{ 0 }, g_handles{ 0 }, g_left{ 0 }, g_failed{ 0 };

		// Held weakly, so the engine's references decide the lifetime.
		std::shared_mutex g_registryMutex;
		std::unordered_map<const org::Resource*, std::weak_ptr<const Texture>> g_registry;

		struct Record final : Texture
		{
			rhi::ResourcePtr image;

			~Record() override
			{
				// The graph texture stays unique until here, so no other record has the key.
				if (graph) {
					std::unique_lock lock(g_registryMutex);
					g_registry.erase(graph.get());
				}
				// The import does not own the image, so no import may find it after this.
				RenderGraph::ImageRegistry::Retire(graph.get());
				// Release the graph's import before the image.
				graph.reset();
				image = {};
				g_live.fetch_sub(1, std::memory_order_relaxed);
			}
		};

		/** @brief The wrap failed after DXVK began it, so its stream may name the image. Kept for the session, since nothing says when DXVK is done. Rare and logged. */
		void Abandon(std::shared_ptr<Record> a_record)
		{
			static std::mutex mutex;
			static std::vector<std::shared_ptr<Record>> abandoned;
			std::scoped_lock lock(mutex);
			abandoned.push_back(std::move(a_record));
		}

		/** @brief Freed once DXVK destroys the texture and the GPU is past it. Also safe if the texture refuses the tag. */
		void DropWithTexture(winrt::com_ptr<ID3D11Resource>& a_texture, std::shared_ptr<Record> a_record)
		{
			Tripwire::AttachGraphResource(a_texture.get(), a_record->graph, std::move(a_record));
			a_texture = nullptr;
		}

		void LogOnce(std::atomic<bool>& a_logged, std::string_view a_name, std::string_view a_why)
		{
			if (!a_logged.exchange(true, std::memory_order_relaxed))
				logger::warn("[ORG] engine textures: {} left to the engine: {} (logged once)", a_name, a_why);
		}

		bool Is3D(const CreateInfo& a_info) { return a_info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D; }

		/** @brief One image entry per mip. */
		org::TextureDescription Describe(const CreateInfo& a_info, rhi::Format a_format)
		{
			const bool cube = (a_info.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
			const UINT slices = Is3D(a_info) ? 1u : (std::max)(a_info.depthOrArraySize, 1u);
			org::TextureDescription desc{};
			desc.type = Is3D(a_info) ? rhi::ResourceType::Texture3D : rhi::ResourceType::Texture2D;
			desc.format = a_format;
			desc.isCubemap = cube;
			desc.arraySize = cube ? slices / 6 : slices;
			desc.isArray = slices > (cube ? 6u : 1u);
			desc.depth = Is3D(a_info) ? a_info.depthOrArraySize : 1u;
			desc.sampleCount = 1;
			desc.hasSRV = true;
			desc.srvFormat = a_format;
			desc.hasUAV = (a_info.bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
			desc.uavFormat = a_format;
			// Importers that clear it need a CPU-side UAV, as the twins and the anchors' imports have.
			desc.hasNonShaderVisibleUAV = desc.hasUAV;
			for (UINT mip = 0; mip < a_info.mipLevels; ++mip)
				desc.imageDimensions.push_back({ (std::max)(a_info.width >> mip, 1u), (std::max)(a_info.height >> mip, 1u), 0, 0 });
			return desc;
		}

		/** @brief Null when it can. */
		const char* Unsupported(const CreateInfo& a_info)
		{
			if (a_info.dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D && !Is3D(a_info))
				return "not a 2D or 3D texture";
			if (!a_info.width || !a_info.height || !a_info.mipLevels || !a_info.depthOrArraySize)
				return "an empty description";
			if (a_info.miscFlags & ~UINT(D3D11_RESOURCE_MISC_TEXTURECUBE))
				return "misc flags other than a cube";
			if (a_info.bindFlags & ~UINT(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS))
				return "bind flags other than SRV and UAV";
			if (DirectX::IsTypeless(a_info.format, false) || rhi::helpers::ToRHI(a_info.format) == rhi::Format::Unknown)
				return "a format BasicRHI has no typed equivalent of";
			return nullptr;
		}

		std::atomic<bool> g_loggedUnsupported{ false }, g_loggedFormat{ false }, g_loggedImage{ false }, g_loggedWrap{ false }, g_loggedLayout{ false },
			g_loggedView{ false }, g_loggedInactive{ false };
	}

	bool Active()
	{
		return Engine::GraphMode() && RenderGraphRuntime::Get().IsActive();
	}

	namespace
	{
		/**
		* @brief Guest mode: a BasicRHI image, which DXVK wraps and fills with the contents in its own stream, imported into the graph
		* in the layout DXVK rests it in. Null on success, else why not.
		*/
		const char* MakeGuest(const CreateInfo& a_info, rhi::Format a_format, const DxvkOrgInteropTextureWrap& a_wrap, const org::TextureDescription& a_graphDesc,
			DxvkOrgInteropResourceInfo& a_out, std::shared_ptr<Record>& a_record, winrt::com_ptr<ID3D11Resource>& a_texture)
		{
			auto& runtime = RenderGraphRuntime::Get();
			auto* host = runtime.Host();
			rhi::ResourceDesc desc{};
			desc.type = Is3D(a_info) ? rhi::ResourceType::Texture3D : rhi::ResourceType::Texture2D;
			desc.heapType = rhi::HeapType::DeviceLocal;
			std::uint32_t flags = rhi::RF_None;
			if (a_info.bindFlags & D3D11_BIND_UNORDERED_ACCESS)
				flags |= rhi::RF_AllowUnorderedAccess;
			if (a_info.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
				flags |= rhi::RF_TextureCubeCompatible;
			desc.resourceFlags = static_cast<rhi::ResourceFlags>(flags);
			desc.debugName = a_info.name.c_str();
			desc.texture.format = a_format;
			desc.texture.width = a_info.width;
			desc.texture.height = a_info.height;
			desc.texture.depthOrLayers = static_cast<std::uint16_t>(a_info.depthOrArraySize);
			desc.texture.mipLevels = static_cast<std::uint16_t>(a_info.mipLevels);
			desc.texture.sampleCount = 1;
			if (host->Device().CreateCommittedResource(desc, a_record->image) != rhi::Result::Ok || !a_record->image)
				return "BasicRHI could not create the image";
			a_texture = runtime.WrapTexture(a_record->image.Get(), a_wrap, a_out, a_info.name.c_str(), a_info.initialData);
			if (!a_texture) {
				// DXVK's stream may name the image already, and no texture is left to tie it to.
				Abandon(std::move(a_record));
				return "DXVK could not wrap the image";
			}
			a_record->graph = RenderGraph::ImportDxvkTexture(host->Device(), a_out.image, a_graphDesc, a_info.name.c_str());
			if (!a_record->graph) {
				DropWithTexture(a_texture, std::move(a_record));
				return "the graph cannot import the layout DXVK rests it in";
			}
			return nullptr;
		}

		/**
		* @brief Native mode: an ORG texture (persistent, never aliased) under a native handle. Its contents go through the graphs' upload
		* service, which copies them ahead of the next work that can use it, so nothing waits here. Without contents the handle's wrap
		* zeroes it. Null on success, else why not.
		*/
		const char* MakeNative(const CreateInfo& a_info, rhi::Format a_format, const DxvkOrgInteropTextureWrap& a_wrap, DxvkOrgInteropResourceInfo& a_out,
			Record& a_record, winrt::com_ptr<ID3D11Resource>& a_texture)
		{
			std::string why;
			if (!Native::CanWrapGraphTexture(a_info.format, a_info.bindFlags, why))
				return "native mode cannot wrap an ORG texture of its format";
			const bool cube = (a_info.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
			const UINT layers = Is3D(a_info) ? 1u : (std::max)(a_info.depthOrArraySize, 1u);
			org::TextureDescription desc{};
			desc.type = Is3D(a_info) ? rhi::ResourceType::Texture3D : rhi::ResourceType::Texture2D;
			desc.format = a_format;
			desc.isCubemap = cube;
			desc.arraySize = cube ? (std::max)(layers / 6, 1u) : layers;
			desc.isArray = layers > (cube ? 6u : 1u);
			desc.depth = Is3D(a_info) ? a_info.depthOrArraySize : 1u;
			desc.hasSRV = true;
			desc.srvFormat = a_format;
			desc.hasUAV = (a_info.bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
			desc.uavFormat = a_format;
			desc.hasNonShaderVisibleUAV = desc.hasUAV;
			desc.channels = 4;
			desc.allowAlias = false;
			// Slice-major, as a PixelBuffer reads them.
			for (UINT slice = 0; slice < layers; ++slice)
				for (UINT mip = 0; mip < a_info.mipLevels; ++mip)
					desc.imageDimensions.push_back({ (std::max)(a_info.width >> mip, 1u), (std::max)(a_info.height >> mip, 1u), 0, 0 });
			std::shared_ptr<org::PixelBuffer> graph;
			try {
				graph = org::PixelBuffer::CreateShared(desc);
				graph->SetName(a_info.name);
			} catch (const std::exception&) {
				return "ORG could not make the texture";
			}
			const bool contents = a_info.initialData != nullptr;
			a_texture = Native::WrapGraphTexture(graph, a_wrap, a_out, a_info.name.c_str(), contents ? Native::WrapFlags::DeferInitialize : Native::WrapFlags::None);
			if (!a_texture)
				return "native mode could not wrap the ORG texture";
			if (contents) {
				auto* host = RenderGraphRuntime::Get().Host();
				const auto& uploads = host ? host->GetRuntime().Uploads() : nullptr;
				if (!uploads)
					return "the graphs have no upload service";
				const UINT subresources = a_info.mipLevels * layers;
				std::vector<rhi::helpers::SubresourceData> sources;
				sources.reserve(subresources);
				for (UINT i = 0; i < subresources; ++i) {
					const auto& source = a_info.initialData[i];
					if (!source.pSysMem)
						return "a subresource has no contents";
					sources.push_back({ source.pSysMem, source.SysMemPitch, source.SysMemSlicePitch });
				}
				uploads->UploadTextureSubresources(org::runtime::UploadTarget::FromShared(graph), a_format, a_info.width, a_info.height,
					Is3D(a_info) ? a_info.depthOrArraySize : 1u, a_info.mipLevels, layers, sources.data(), subresources, __FILE__, __LINE__);
			}
			a_record.graph = std::move(graph);
			return nullptr;
		}
	}

	bool Create(const CreateInfo& a_info, Created& a_out)
	{
		a_out = {};
		if (!Engine::GraphMode())
			return false;
		auto& runtime = RenderGraphRuntime::Get();
		auto* host = runtime.Host();
		if (!Active() || !host) {
			// Made before the render graph started.
			g_left.fetch_add(1, std::memory_order_relaxed);
			LogOnce(g_loggedInactive, a_info.name, "the render graph cannot wrap images with their contents yet");
			return false;
		}
		if (const char* why = Unsupported(a_info)) {
			g_left.fetch_add(1, std::memory_order_relaxed);
			LogOnce(g_loggedUnsupported, a_info.name, why);
			return false;
		}
		const auto fail = [&](std::atomic<bool>& a_logged, std::string_view a_why) {
			g_failed.fetch_add(1, std::memory_order_relaxed);
			if (!a_logged.exchange(true, std::memory_order_relaxed))
				logger::error("[ORG] engine textures: {}: {}; it and others like it are the engine's (logged once)", a_info.name, a_why);
			return false;
		};

		// BasicRHI stores formats the device lacks the way DXVK does, such as A8 as R8, so both agree.
		// What neither can sample, or store to for a UAV, stays the engine's.
		const auto format = rhi::helpers::ToRHI(a_info.format);
		rhi::FormatSupport support{};
		const std::uint32_t needed = rhi::FU_ShaderResource | ((a_info.bindFlags & D3D11_BIND_UNORDERED_ACCESS) ? rhi::FU_UnorderedAccess : rhi::FU_None);
		if (host->Device().QueryFormatSupport(format, support) != rhi::Result::Ok || (support.usage & needed) != needed) {
			g_left.fetch_add(1, std::memory_order_relaxed);
			LogOnce(g_loggedFormat, a_info.name, "the device cannot sample (or store to) its format");
			return false;
		}
		DxvkOrgInteropTextureWrap wrap{};
		wrap.dimension = static_cast<std::uint32_t>(a_info.dimension);
		wrap.width = a_info.width;
		wrap.height = a_info.height;
		wrap.depthOrArraySize = a_info.depthOrArraySize;
		wrap.mipLevels = a_info.mipLevels;
		wrap.format = static_cast<std::uint32_t>(a_info.format);
		wrap.sampleCount = 1;
		wrap.bindFlags = a_info.bindFlags;
		wrap.miscFlags = a_info.miscFlags;
		DxvkOrgInteropResourceInfo info{};
		winrt::com_ptr<ID3D11Resource> texture;
		auto record = std::make_shared<Record>();
		g_live.fetch_add(1, std::memory_order_relaxed);
		const auto graphDesc = Describe(a_info, format);
		if (Native::Active()) {
			if (const char* why = MakeNative(a_info, format, wrap, info, *record, texture))
				return fail(g_loggedImage, why);
		} else if (const char* why = MakeGuest(a_info, format, wrap, graphDesc, info, record, texture)) {
			return fail(g_loggedImage, why);
		}
		// An import of this texture returns this resource instead of importing it again.
		RenderGraph::ImageRegistry::Register(reinterpret_cast<std::uint64_t>(info.image.image), { record->graph, nullptr,
			std::make_shared<const org::TextureDescription>(graphDesc), "EngineTextures", a_info.name, RenderGraph::ImageRegistry::Kind::Made });
		record->name = a_info.name;
		record->dimension = a_info.dimension;
		record->desc.Width = a_info.width;
		record->desc.Height = a_info.height;
		record->desc.MipLevels = a_info.mipLevels;
		record->desc.ArraySize = a_info.depthOrArraySize;
		record->desc.Format = a_info.format;
		record->desc.SampleDesc.Count = 1;
		record->desc.Usage = D3D11_USAGE_DEFAULT;
		record->desc.BindFlags = a_info.bindFlags;
		record->desc.MiscFlags = a_info.miscFlags;
		record->components[0] = info.image.components.r;
		record->components[1] = info.image.components.g;
		record->components[2] = info.image.components.b;
		record->components[3] = info.image.components.a;

		winrt::com_ptr<ID3D11ShaderResourceView> view;
		if (FAILED(globals::d3d::device->CreateShaderResourceView(texture.get(), &a_info.srv, view.put()))) {
			DropWithTexture(texture, std::move(record));
			return fail(g_loggedView, "D3D11 refused the view");
		}
		Util::SetResourceName(view.get(), "%s SRV", a_info.name.c_str());

		// Attach before anything else holds the objects. The record is freed once DXVK destroyed both and the GPU is past them.
		if (!Tripwire::AttachGraphResource(view.get(), record->graph, record)) {
			view = nullptr;
			DropWithTexture(texture, std::move(record));
			return fail(g_loggedView, "the D3D11 view did not take private data");
		}
		if (!Tripwire::AttachGraphResource(texture.get(), record->graph, record)) {
			// The view's tag holds the record, and the view holds the texture.
			view = nullptr;
			texture = nullptr;
			return fail(g_loggedView, "the D3D11 texture did not take private data");
		}

		// Handles only once every engine bind goes through the guest context.
		winrt::com_ptr<ID3D11Resource> textureHandle;
		winrt::com_ptr<ID3D11ShaderResourceView> viewHandle;
		if (Tripwire::GuestContextInstalled()) {
			Tripwire::Options options;
			options.name = a_info.name;
			options.graph = record->graph;
			options.owner = record;
			if (Is3D(a_info)) {
				D3D11_TEXTURE3D_DESC desc3D{ a_info.width, a_info.height, a_info.depthOrArraySize, a_info.mipLevels, a_info.format, D3D11_USAGE_DEFAULT,
					a_info.bindFlags, 0, a_info.miscFlags };
				textureHandle.copy_from(Tripwire::CreateTexture3D(desc3D, options, texture.as<ID3D11Texture3D>().get()).get());
			} else {
				textureHandle.copy_from(Tripwire::CreateTexture2D(record->desc, options, texture.as<ID3D11Texture2D>().get()).get());
			}
			options.name = a_info.name + " SRV";
			viewHandle = Tripwire::CreateShaderResourceView(textureHandle.get(), a_info.srv, options, view.get());
			g_handles.fetch_add(1, std::memory_order_relaxed);
		}

		{
			std::unique_lock lock(g_registryMutex);
			g_registry[record->graph.get()] = record;
		}
		// Made with contents and SRV-only, so immutable from here. A zeroed texture gets filled later, so it is not.
		if (RenderGraph::UndeclaredAccess::On() && a_info.initialData && a_info.initialData->pSysMem && a_info.bindFlags == D3D11_BIND_SHADER_RESOURCE)
			RenderGraph::UndeclaredAccess::NoteImmutable(record->graph);
		g_created.fetch_add(1, std::memory_order_relaxed);
		if (viewHandle) {
			a_out.texture = std::move(textureHandle);
			a_out.view = std::move(viewHandle);
		} else {
			a_out.texture = std::move(texture);
			a_out.view = std::move(view);
		}
		return true;
	}

	std::shared_ptr<const Texture> Find(const ID3D11ShaderResourceView* a_view)
	{
		if (!a_view || !Engine::GraphMode())
			return nullptr;
		auto graph = Tripwire::GraphResourceOf(a_view);
		if (!graph) {
			// Scaleform makes its own view of every texture it is given, so fall back to the view's texture.
			winrt::com_ptr<ID3D11Resource> resource;
			const_cast<ID3D11ShaderResourceView*>(a_view)->GetResource(resource.put());
			graph = Tripwire::GraphResourceOf(resource.get());
		}
		if (!graph)
			return nullptr;
		std::shared_lock lock(g_registryMutex);
		const auto found = g_registry.find(graph.get());
		return found == g_registry.end() ? nullptr : found->second.lock();
	}

	Stats GetStats()
	{
		Stats stats;
		stats.created = g_created.load(std::memory_order_relaxed);
		stats.live = g_live.load(std::memory_order_relaxed);
		stats.handles = g_handles.load(std::memory_order_relaxed);
		stats.left = g_left.load(std::memory_order_relaxed);
		stats.failed = g_failed.load(std::memory_order_relaxed);
		return stats;
	}
}


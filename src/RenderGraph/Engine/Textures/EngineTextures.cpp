#include "RenderGraph/Engine/Textures/EngineTextures.h"

#include "RenderGraph/Switches.h"

namespace RenderGraph::Engine::Textures
{
	bool Requested()
	{
		// The creators were checked on SE 1.5.97 and AE 1.6.1170, not on VR.
		static const bool requested = !REL::Module::IsVR() && Switches::On("CS_ORG_ENGINE_TEXTURES");
		return requested;
	}
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Globals.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "Utils/D3D.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/TextureDescription.h>
#	include <rhi_helpers.h>

#	include <DirectXTex.h>

#	include <algorithm>
#	include <atomic>
#	include <mutex>
#	include <shared_mutex>
#	include <unordered_map>
#	include <vector>

namespace RenderGraph::Engine::Tripwire
{
	// P4.1 (Tripwire/GuestDevice.h): whether the engine's device and context pointers hold the guest wrapping handles, so a handle
	// in an engine field reaches DXVK only unwrapped.
	bool GuestContextInstalled();
}

namespace RenderGraph::Engine::Textures
{
	namespace
	{
		namespace Tripwire = RenderGraph::Engine::Tripwire;

		// {6F0B2C41-9D3E-4A57-B1C8-2E7D5A3F9C04}: the sentinel each texture's D3D11 texture and view carry.
		constexpr GUID kSentinelGuid = { 0x6f0b2c41, 0x9d3e, 0x4a57, { 0xb1, 0xc8, 0x2e, 0x7d, 0x5a, 0x3f, 0x9c, 0x04 } };

		std::atomic<std::uint64_t> g_created{ 0 }, g_live{ 0 }, g_handles{ 0 }, g_left{ 0 }, g_failed{ 0 };

		/** @brief A texture's record: the image ORG owns, and what Texture says about it. */
		struct Record final : Texture
		{
			rhi::ResourcePtr image;

			~Record() override
			{
				// The graph's import of the image first, then the image.
				graph.reset();
				image = {};
				g_live.fetch_sub(1, std::memory_order_relaxed);
			}
		};

		// Views (D3D11 views and their tripwire handles) to their records. Held weakly: the engine's references decide the lifetime.
		std::shared_mutex g_registryMutex;
		std::unordered_map<const void*, std::weak_ptr<Record>> g_registry;

		// What released textures left, for the render thread's ReleaseAfterD3D11.
		std::mutex g_retiredMutex;
		std::vector<std::shared_ptr<const void>> g_retired;

		void Retire(std::shared_ptr<const void> a_owner)
		{
			std::scoped_lock lock(g_retiredMutex);
			g_retired.push_back(std::move(a_owner));
		}

		void Forget(const void* a_key, const Record* a_record)
		{
			std::unique_lock lock(g_registryMutex);
			const auto found = g_registry.find(a_key);
			if (found == g_registry.end())
				return;
			// A newer texture's view at the same address keeps its entry.
			const auto live = found->second.lock();
			if (!live || live.get() == a_record)
				g_registry.erase(found);
		}

		/**
		 * @brief Private data on a D3D11 texture or view: its object's destruction (on whatever thread drops the last reference) is
		 * its last Release. That queues the record for ReleaseAfterD3D11, and a view's also leaves the registry.
		 */
		class Sentinel final : public IUnknown
		{
		public:
			Sentinel(std::shared_ptr<Record> a_record, const void* a_view, const void* a_handle) :
				record(std::move(a_record)), view(a_view), handle(a_handle) {}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_iid, void** a_out) override
			{
				if (!a_out)
					return E_POINTER;
				if (a_iid == __uuidof(IUnknown)) {
					*a_out = static_cast<IUnknown*>(this);
					AddRef();
					return S_OK;
				}
				*a_out = nullptr;
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1, std::memory_order_relaxed) + 1; }

			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG left = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
				if (left == 0) {
					if (view)
						Forget(view, record.get());
					if (handle)
						Forget(handle, record.get());
					Retire(std::move(record));
					delete this;
				}
				return left;
			}

			/** @brief Attaches a new sentinel to a_object; false when it does not take private data. */
			static bool Attach(ID3D11DeviceChild* a_object, std::shared_ptr<Record> a_record, const void* a_view, const void* a_handle)
			{
				auto* sentinel = new Sentinel(std::move(a_record), a_view, a_handle);
				const HRESULT result = a_object->SetPrivateDataInterface(kSentinelGuid, sentinel);
				sentinel->Release();  // the object holds it now, or nothing does
				return SUCCEEDED(result);
			}

		private:
			std::atomic<ULONG> refs{ 1 };
			std::shared_ptr<Record> record;
			const void* view;
			const void* handle;
		};

		/** @brief Logs a reason once: the first texture it applies to names it, the counters count the rest. */
		void LogOnce(std::atomic<bool>& a_logged, std::string_view a_name, std::string_view a_why)
		{
			if (!a_logged.exchange(true, std::memory_order_relaxed))
				logger::warn("[ORG] engine textures: {} left to the engine: {} (logged once)", a_name, a_why);
		}

		bool Is3D(const CreateInfo& a_info) { return a_info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D; }

		/** @brief The graph's description of an external texture: one image entry per mip. */
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
			for (UINT mip = 0; mip < a_info.mipLevels; ++mip)
				desc.imageDimensions.push_back({ (std::max)(a_info.width >> mip, 1u), (std::max)(a_info.height >> mip, 1u), 0, 0 });
			return desc;
		}

		/** @brief Why a description cannot be made here, or null when it can. */
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

		std::atomic<bool> g_loggedUnsupported{ false }, g_loggedImage{ false }, g_loggedWrap{ false }, g_loggedLayout{ false },
			g_loggedView{ false }, g_loggedInactive{ false };
	}

	bool Active()
	{
		return Requested() && RenderGraphRuntime::Get().CanWrapTexturesWithData();
	}

	bool Create(const CreateInfo& a_info, Created& a_out)
	{
		a_out = {};
		if (!Requested())
			return false;
		auto& runtime = RenderGraphRuntime::Get();
		auto* host = runtime.Host();
		if (!Active() || !host) {
			// Textures made before the render graph started, or with a DXVK build without the export.
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

		// The image, which ORG owns.
		const auto format = rhi::helpers::ToRHI(a_info.format);
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
		desc.texture.format = format;
		desc.texture.width = a_info.width;
		desc.texture.height = a_info.height;
		desc.texture.depthOrLayers = static_cast<std::uint16_t>(a_info.depthOrArraySize);
		desc.texture.mipLevels = static_cast<std::uint16_t>(a_info.mipLevels);
		desc.texture.sampleCount = 1;
		auto record = std::make_shared<Record>();
		g_live.fetch_add(1, std::memory_order_relaxed);
		if (host->Device().CreateCommittedResource(desc, record->image) != rhi::Result::Ok || !record->image)
			return fail(g_loggedImage, "BasicRHI could not create the image");

		// The D3D11 texture on it, with its contents uploaded in DXVK's stream.
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
		auto texture = runtime.WrapTexture(record->image.Get(), wrap, info, a_info.name.c_str(), a_info.initialData);
		if (!texture) {
			// DXVK's stream may name the image already: it goes once D3D11 is past this point.
			Retire(std::move(record));
			return fail(g_loggedWrap, "DXVK could not wrap the image");
		}

		// The graph's view of it, in the layout DXVK rests it in.
		record->graph = RenderGraph::ImportDxvkTexture(host->Device(), info.image, Describe(a_info, format), a_info.name.c_str());
		if (!record->graph) {
			texture = nullptr;
			Retire(std::move(record));
			return fail(g_loggedLayout, "the graph cannot import the layout DXVK rests it in");
		}
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
			texture = nullptr;
			Retire(std::move(record));
			return fail(g_loggedView, "D3D11 refused the view");
		}
		Util::SetResourceName(view.get(), "%s SRV", a_info.name.c_str());

		// Tripwire handles in the engine's fields, once every engine bind goes through the guest context.
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

		// The sentinels: the D3D11 objects' destruction retires the record; the view's also leaves the registry.
		if (!Sentinel::Attach(texture.get(), record, nullptr, nullptr) || !Sentinel::Attach(view.get(), record, view.get(), viewHandle.get())) {
			viewHandle = nullptr;
			textureHandle = nullptr;
			view = nullptr;
			texture = nullptr;
			Retire(std::move(record));
			return fail(g_loggedView, "the D3D11 objects did not take private data");
		}
		{
			std::unique_lock lock(g_registryMutex);
			g_registry[view.get()] = record;
			if (viewHandle)
				g_registry[viewHandle.get()] = record;
		}
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
		if (!a_view || !Requested())
			return nullptr;
		std::shared_ptr<Record> record;
		{
			std::shared_lock lock(g_registryMutex);
			const auto found = g_registry.find(a_view);
			if (found == g_registry.end())
				return nullptr;
			record = found->second.lock();
		}
		// A handle's entry leaves with its view's sentinel, which a D3D11 binding can outlive the handle by: a handle made since at
		// that address must stand for this record.
		if (record && Tripwire::IsHandle(a_view) && Tripwire::Describe(a_view).graph != record->graph)
			return nullptr;
		return record;
	}

	void ServiceRetired()
	{
		std::vector<std::shared_ptr<const void>> retired;
		{
			std::scoped_lock lock(g_retiredMutex);
			retired.swap(g_retired);
		}
		auto& runtime = RenderGraphRuntime::Get();
		for (auto& owner : retired)
			runtime.ReleaseAfterD3D11(std::move(owner));
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

#else  // !CS_HAS_RENDER_GRAPH

namespace RenderGraph::Engine::Textures
{
	bool Active() { return false; }
	bool Create(const CreateInfo&, Created& a_out)
	{
		a_out = {};
		return false;
	}
	std::shared_ptr<const Texture> Find(const ID3D11ShaderResourceView*) { return nullptr; }
	void ServiceRetired() {}
	Stats GetStats() { return {}; }
}

#endif

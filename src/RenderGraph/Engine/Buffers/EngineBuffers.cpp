// The engine's static vertex and index buffers on ORG, and what both halves share (EngineBuffers.h).

#include "RenderGraph/Engine/Buffers/EngineBuffers.h"

#include "RenderGraph/Switches.h"

namespace RenderGraph::Engine::Buffers
{
	bool Requested()
	{
		static const bool requested = Switches::On("CS_ORG_ENGINE_BUFFERS");
		return requested;
	}
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/Engine/Buffers/BufferBacking.h"
#	include "RenderGraph/Engine/Tripwire/GuestDevice.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Resources/Buffers/Buffer.h>
#	include <rhi.h>

#	include <cstring>
#	include <exception>
#	include <string_view>

namespace RenderGraph::Engine::Buffers
{
	namespace detail
	{
		namespace
		{
			// Each failure kind is logged once: a load fails the same way for every buffer.
			void WarnOnce(std::atomic_flag& a_flag, std::string_view a_what)
			{
				if (!a_flag.test_and_set())
					logger::warn("[ORG] engine buffers: {}; those buffers stay D3D11", a_what);
			}
			std::atomic_flag warnedMap, warnedWrap, warnedAllocate, warnedTie;
		}

		std::optional<Backing> MakeBacking(const D3D11_BUFFER_DESC& a_desc, const char* a_name)
		{
			auto& runtime = RenderGraphRuntime::Get();
			if (!runtime.IsActive() || !a_desc.ByteWidth)
				return std::nullopt;
			try {
				// Made on loader threads too: the host's world is the render thread's (as DCLF's pooled backings).
				org::Resource::ScopedECSRegistrationSuppression suppressECS;
				// Device-local and host-visible with resizable BAR, host memory otherwise: the CPU fills it with no GPU copy.
				auto buffer = org::Buffer::CreateShared(rhi::HeapType::GPUUpload, a_desc.ByteWidth);
				buffer->SetName(a_name);
				void* data = nullptr;
				auto api = buffer->GetAPIResource();
				api.Map(&data);  // kept mapped for the buffer's life
				if (!data) {
					WarnOnce(warnedMap, "an upload buffer has no mapping");
					return std::nullopt;
				}
				D3D11_BUFFER_DESC wrap{};
				wrap.ByteWidth = a_desc.ByteWidth;
				wrap.Usage = D3D11_USAGE_DEFAULT;
				wrap.BindFlags = a_desc.BindFlags;
				auto guest = runtime.WrapBuffer(*buffer, wrap);
				if (!guest) {
					WarnOnce(warnedWrap, "DXVK refused to wrap an upload buffer");
					return std::nullopt;
				}
				return Backing{ std::move(buffer), std::move(guest), static_cast<std::uint8_t*>(data) };
			} catch (const std::exception& e) {
				WarnOnce(warnedAllocate, fmt::format("an upload buffer could not be made ({})", e.what()));
				return std::nullopt;
			}
		}

		winrt::com_ptr<ID3D11Buffer> EngineView(const D3D11_BUFFER_DESC& a_desc, const Backing& a_backing, const char* a_name)
		{
			std::shared_ptr<org::Resource> graph = a_backing.buffer;
			// A handle only where the engine's context unwraps it: DXVK's own context casts what it is given.
			if (Tripwire::GuestContextInstalled()) {
				Tripwire::Options options;
				options.name = a_name;
				options.graph = std::move(graph);
				return Tripwire::CreateBuffer(a_desc, std::move(options), a_backing.guest.get());
			}
			if (!Tripwire::AttachGraphResource(a_backing.guest.get(), std::move(graph))) {
				WarnOnce(warnedTie, "an upload buffer could not be tied to its DXVK buffer");
				return nullptr;
			}
			return a_backing.guest;
		}

		Counters& GetCounters()
		{
			static Counters counters;
			return counters;
		}
	}

	namespace
	{
		// Above zero while this thread runs one of the geometry facade's creators: its CreateBuffer calls are the engine's VB/IB.
		thread_local std::uint32_t creatingGeometry = 0;

		/** @brief A geometry creator (75473, 75476, 75481): every one takes five arguments, passed through unchanged. */
		template <int Creator>
		struct CreateGeometry
		{
			static void* thunk(void* a_renderer, void* a_source, std::uint64_t a_desc, std::uint64_t a_count, std::uint64_t a_indexCount)
			{
				++creatingGeometry;
				void* const shape = func(a_renderer, a_source, a_desc, a_count, a_indexCount);
				--creatingGeometry;
				return shape;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The engine's static VB or IB: DEFAULT, one bind flag, no CPU access, and its contents given (R6.5). */
		bool IsStaticGeometry(const D3D11_BUFFER_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data)
		{
			return a_data && a_data->pSysMem && a_desc.ByteWidth && a_desc.Usage == D3D11_USAGE_DEFAULT && !a_desc.CPUAccessFlags &&
			       !a_desc.MiscFlags && (a_desc.BindFlags == D3D11_BIND_VERTEX_BUFFER || a_desc.BindFlags == D3D11_BIND_INDEX_BUFFER);
		}

		winrt::com_ptr<ID3D11Buffer> MakeStatic(const D3D11_BUFFER_DESC& a_desc, const void* a_contents)
		{
			const char* name = a_desc.BindFlags == D3D11_BIND_VERTEX_BUFFER ? "Engine::TriShape VB" : "Engine::TriShape IB";
			auto backing = detail::MakeBacking(a_desc, name);
			if (!backing)
				return nullptr;
			// The writes are done before the engine can draw the buffer, and a queue submission makes host writes visible.
			std::memcpy(backing->data, a_contents, a_desc.ByteWidth);
			auto view = detail::EngineView(a_desc, *backing, name);
			if (view) {
				auto& counters = detail::GetCounters();
				counters.created.fetch_add(1, std::memory_order_relaxed);
				counters.createdBytes.fetch_add(a_desc.ByteWidth, std::memory_order_relaxed);
			}
			return view;
		}

		/** @brief ID3D11Device::CreateBuffer on DXVK's device: a geometry creator's VB and IB are made on ORG. */
		struct Device_CreateBuffer
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* a_this, const D3D11_BUFFER_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data,
				ID3D11Buffer** a_buffer)
			{
				if (creatingGeometry && a_buffer && a_desc && IsStaticGeometry(*a_desc, a_data)) {
					if (auto made = MakeStatic(*a_desc, a_data->pSysMem)) {
						*a_buffer = made.detach();
						return S_OK;
					}
					detail::GetCounters().fallbacks.fetch_add(1, std::memory_order_relaxed);
				}
				return func(a_this, a_desc, a_data, a_buffer);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallGameHooks()
	{
		if (!Requested())
			return;
		logger::info("[ORG] engine buffers (CS_ORG_ENGINE_BUFFERS=1): hooking the geometry creators");
		stl::detour_thunk<CreateGeometry<0>>(REL::RelocationID(75473, 77259));  // CreateTriShape
		stl::detour_thunk<CreateGeometry<1>>(REL::RelocationID(75476, 77262));  // CreateTriShapeSharedVertices
		stl::detour_thunk<CreateGeometry<2>>(REL::RelocationID(75481, 77268));  // CreateDynamicTriShapeData
	}

	void InstallDeviceHooks(ID3D11Device* a_device)
	{
		if (!Requested() || !a_device)
			return;
		stl::detour_vfunc<3, Device_CreateBuffer>(a_device);
	}

	std::optional<Direct> Resolve(ID3D11Buffer* a_buffer)
	{
		if (!a_buffer || !Requested())
			return std::nullopt;
		const auto buffer = std::dynamic_pointer_cast<org::Buffer>(Tripwire::GraphResourceOf(a_buffer));
		if (!buffer)
			return std::nullopt;
		const auto api = buffer->GetAPIResource();
		rhi::VulkanResourceInfo info{};
		if (!rhi::vulkan::get_resource_info(api, info) || !info.resource)
			return std::nullopt;
		Direct direct;
		direct.vkBuffer = reinterpret_cast<std::uint64_t>(rhi::vulkan::from_native_void<VkBuffer>(info.resource));
		direct.size = buffer->GetBufferSize();
		if (auto* host = RenderGraphRuntime::Get().Host())
			direct.address = host->Device().GetBufferDeviceAddress({ api.GetHandle(), 0 });
		return direct;
	}

	Stats GetStats()
	{
		const auto& counters = detail::GetCounters();
		Stats stats;
		stats.created = counters.created.load(std::memory_order_relaxed);
		stats.createdBytes = counters.createdBytes.load(std::memory_order_relaxed);
		stats.fallbacks = counters.fallbacks.load(std::memory_order_relaxed);
		return stats;
	}
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: the engine's buffers stay D3D11.
namespace RenderGraph::Engine::Buffers
{
	void InstallGameHooks()
	{
		if (Requested())
			logger::warn("[ORG] CS_ORG_ENGINE_BUFFERS=1 ignored: built without OpenRenderGraph");
	}
	void InstallDeviceHooks(ID3D11Device*) {}
	std::optional<Direct> Resolve(ID3D11Buffer*) { return std::nullopt; }
	Stats GetStats() { return {}; }
}

#endif

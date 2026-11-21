#pragma once

// The tripwire handle classes. Only the tripwire's own sources include this. Ports use Tripwire.h.

#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Engine/Tripwire/TripwireInterfaces.inl"

#include <array>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace RenderGraph::Engine::Tripwire
{
	namespace Detail
	{
		extern std::atomic<ContextObserver> contextObserver;

		extern std::atomic<const NativeServices*> nativeServices;

		extern std::atomic<const MapHooks*> mapHooks;

		extern std::atomic<GuestCopyHook> guestCopyHook;
	}

	/** @brief What a handle stands for besides its wrapped object. Immutable; Retarget swaps in a new one and retires the old. */
	struct HandleState
	{
		std::shared_ptr<org::Resource> graph;
		std::shared_ptr<const void> owner;
	};

	class HandleCore
	{
	public:
		HandleCore() = default;
		HandleCore(const HandleCore&) = delete;
		HandleCore& operator=(const HandleCore&) = delete;
		/** @brief Releases the wrapped object and queues the ORG resource and owner for ServiceRetired. */
		~HandleCore();

		/** @brief Set once by the factory, before the handle is handed out. */
		void Init(Kind a_kind, Options&& a_options, IUnknown* a_wrap);

		/** @brief Starts every tripwire method. Records the call when watched, and returns whether to forward it. */
		bool Enter(std::uint16_t a_slot, const char* a_method, void* a_caller)
		{
			const bool forward = Wrapped() != nullptr;
			if (!forward || watchAll)
				Record(a_slot, a_method, a_caller, forward, {});
			return forward;
		}

		/** @brief A context call that reaches nothing, in either mode. The observer sees it first, then it is recorded. */
		void Trip(std::uint16_t a_slot, const char* a_method, void* a_caller) const
		{
			if (const auto observer = Detail::contextObserver.load(std::memory_order_relaxed))
				observer(a_slot, a_caller);
			Record(a_slot, a_method, a_caller, false, {});
		}

		/** @brief Counts a call at a_caller. The first one from there is logged with a_detail. */
		void Record(std::uint16_t a_slot, const char* a_method, void* a_caller, bool a_forwarded, std::string_view a_detail) const;

		/** @brief Counts a forwarded call that passed a handle as an argument, a reader that still goes through D3D11. */
		void RecordArgument(std::uint16_t a_slot, const char* a_method, void* a_caller, const HandleCore& a_argument) const;

		std::string Name() const;
		void SetName(std::string a_name);

		/** @brief The wrapped object. Retarget may swap it; the old one lives until ServiceRetired, so a stale load stays valid. */
		IUnknown* Wrapped() const { return wrapped.load(std::memory_order_acquire); }
		/** @brief The ORG resource the handle stands for now. */
		std::shared_ptr<org::Resource> Graph() const
		{
			const auto* current = state.load(std::memory_order_acquire);
			return current ? current->graph : nullptr;
		}
		/** @brief What the handle keeps alive now. */
		std::shared_ptr<const void> Owner() const
		{
			const auto* current = state.load(std::memory_order_acquire);
			return current ? current->owner : nullptr;
		}

		Kind kind = Kind::Count;
		std::atomic<ULONG> refs{ 1 };
		std::atomic<IUnknown*> wrapped{ nullptr };
		/** @brief Guest mode with CS_ORG_TRIPWIRE_WATCH_ALL: every forwarded call is recorded too. */
		bool watchAll = false;
		/** @brief Null when the handle has neither a graph resource nor an owner. Owned by the core; a replaced one is retired. */
		std::atomic<const HandleState*> state{ nullptr };
		/** @brief Nothing is behind the handle (CreateUnbacked*, or Retarget to null). */
		std::atomic<bool> unbacked{ false };

		/** @brief A native handle's private-data interfaces, held as D3D11 holds them until the handle goes or the GUID is set again. */
		std::vector<std::pair<GUID, winrt::com_ptr<IUnknown>>> privateInterfaces;
		std::vector<std::pair<GUID, std::vector<std::uint8_t>>> privateData;
		std::mutex privateLock;

	private:
		mutable std::shared_mutex nameLock;
		std::string name;
	};

	namespace Detail
	{
		template <class R>
		R Failure()
		{
			if constexpr (std::is_void_v<R>)
				return;
			else if constexpr (std::is_same_v<R, HRESULT>)
				return E_FAIL;
			else
				return R{};
		}

		template <class T>
		void Zero(T* a_out)
		{
			if (a_out)
				*a_out = T{};
		}

		template <class T, class N>
		void ZeroN(T* a_out, N a_count)
		{
			if (a_out)
				for (N i = 0; i < a_count; ++i)
					a_out[i] = T{};
		}

		template <class T>
		void ZeroAt(T* a_out, const UINT* a_count)
		{
			if (a_out && a_count)
				ZeroN(a_out, *a_count);
		}

		inline void ZeroBytes(void* a_out, UINT a_size)
		{
			if (a_out && a_size)
				std::memset(a_out, 0, a_size);
		}

		struct CallSite
		{
			const HandleCore* core = nullptr;
			std::uint16_t slot = 0;
			const char* method = nullptr;
			void* caller = nullptr;
		};

		void NoteHandleArgument(const CallSite& a_site, const void* a_handle);

		/** @brief Unwraps one argument of a forwarded call, recording a handle at a_site first. */
		template <class T>
		T* UnwrapAt(T* a_object, const CallSite& a_site)
		{
			if (!a_object || !IsHandle(a_object))
				return a_object;
			NoteHandleArgument(a_site, a_object);
			return Unwrap(a_object);
		}

		/** @brief Registers a handle class's vtable so IsHandle recognises its objects. */
		void RegisterHandleClass(Kind a_kind, const void* a_vtable, HandleCore* (*a_core)(void*));

		HandleCore* CoreOf(const void* a_object);

		/** @brief For the self-test. The calls counted for a_kind's a_method, after which every site is cleared. */
		std::uint64_t Calls(Kind a_kind, std::string_view a_method);
		void ClearSites();
		void SetSelfTestRunning(bool a_running);
	}
}

// clang-format off
#define TW_STRIP(...) __VA_ARGS__
#define TW_ZERO(p) ::RenderGraph::Engine::Tripwire::Detail::Zero(p);
#define TW_ZERO_N(p, n) ::RenderGraph::Engine::Tripwire::Detail::ZeroN(p, n);
#define TW_ZERO_N_AT(p, n) ::RenderGraph::Engine::Tripwire::Detail::ZeroAt(p, n);
#define TW_ZERO_BYTES(p, n) ::RenderGraph::Engine::Tripwire::Detail::ZeroBytes(p, n);
#define TW_UNWRAP(p) ::RenderGraph::Engine::Tripwire::Detail::UnwrapAt(p, twSite)

#define TW_SLOT_ENUM(Ret, Name, Params, Args, Fail) Name,
#define TW_SLOT_NAME(Ret, Name, Params, Args, Fail) #Name,
// Recorded as Enter decides, forwarded when an object is wrapped, else failed.
#define TW_TRIP(Ret, Name, Params, Args, Fail)                                                         \
	Ret STDMETHODCALLTYPE Name Params override                                                         \
	{                                                                                                  \
		void* const twCaller = _ReturnAddress();                                                       \
		if (core.Enter(static_cast<std::uint16_t>(Slot::Name), #Name, twCaller)) {                     \
			[[maybe_unused]] const ::RenderGraph::Engine::Tripwire::Detail::CallSite twSite{ &core,    \
				static_cast<std::uint16_t>(Slot::Name), #Name, twCaller };                             \
			return static_cast<Interface*>(core.Wrapped())->Name Args;                                 \
		}                                                                                              \
		TW_STRIP Fail return ::RenderGraph::Engine::Tripwire::Detail::Failure<Ret>();                  \
	}

// The context forwards nothing it does not implement itself, in either mode. Its Args are never expanded.
#define TW_TRIP_CONTEXT(Ret, Name, Params, Args, Fail)                                                 \
	Ret STDMETHODCALLTYPE Name Params override                                                         \
	{                                                                                                  \
		core.Trip(static_cast<std::uint16_t>(Slot::Name), #Name, _ReturnAddress());                    \
		TW_STRIP Fail return ::RenderGraph::Engine::Tripwire::Detail::Failure<Ret>();                  \
	}

// Each entry is an override, so a missed slot leaves the class abstract and the factories' static_asserts catch it.
#define TW_DECLARE_TRIPS(Iface) TW_DECLARE_TRIPS_AS(Iface, TW_TRIP)
#define TW_DECLARE_TRIPS_AS(Iface, TripMacro)                                                                         \
	class Iface##Trips : public Iface                                                                                 \
	{                                                                                                                 \
	public:                                                                                                           \
		using Interface = Iface;                                                                                      \
		enum class Slot : std::uint16_t { QueryInterface, AddRef, Release, TW_SLOTS_##Iface(TW_SLOT_ENUM) Count };   \
		static_assert(static_cast<std::uint16_t>(Slot::Count) == TW_SLOT_COUNT_##Iface);                             \
		static_assert(static_cast<std::size_t>(Slot::Count) <= ::RenderGraph::Engine::Tripwire::kMaxMethodSlots); \
		static constexpr const char* kSlotNames[] = { "QueryInterface", "AddRef", "Release",                          \
			TW_SLOTS_##Iface(TW_SLOT_NAME) };                                                                         \
		static inline const IID kIids[] = { TW_IIDS_##Iface };                                                        \
		TW_SLOTS_##Iface(TripMacro)                                                                                   \
	protected:                                                                                                        \
		Iface##Trips() = default;                                                                                     \
		::RenderGraph::Engine::Tripwire::HandleCore core;                                                             \
	};
// clang-format on

namespace RenderGraph::Engine::Tripwire
{
	TW_DECLARE_TRIPS(ID3D11Texture1D)
	TW_DECLARE_TRIPS(ID3D11Texture2D)
	TW_DECLARE_TRIPS(ID3D11Texture3D)
	TW_DECLARE_TRIPS(ID3D11Buffer)
	TW_DECLARE_TRIPS(ID3D11ShaderResourceView)
	TW_DECLARE_TRIPS(ID3D11RenderTargetView)
	TW_DECLARE_TRIPS(ID3D11DepthStencilView)
	TW_DECLARE_TRIPS(ID3D11UnorderedAccessView)
	TW_DECLARE_TRIPS(ID3D11VertexShader)
	TW_DECLARE_TRIPS(ID3D11PixelShader)
	TW_DECLARE_TRIPS(ID3D11ComputeShader)
	TW_DECLARE_TRIPS(ID3D11InputLayout)
	TW_DECLARE_TRIPS(ID3D11BlendState)
	TW_DECLARE_TRIPS(ID3D11DepthStencilState)
	TW_DECLARE_TRIPS(ID3D11RasterizerState)
	TW_DECLARE_TRIPS(ID3D11SamplerState)
	TW_DECLARE_TRIPS(ID3D11Query)
	// The context's trips name parameters they never read.
#pragma warning(push)
#pragma warning(disable: 4100)
	TW_DECLARE_TRIPS_AS(ID3D11DeviceContext, TW_TRIP_CONTEXT)
#pragma warning(pop)
	TW_DECLARE_TRIPS(ID3D11Device)
	TW_DECLARE_TRIPS(IDXGISwapChain)

	namespace Detail
	{
		bool IsDebugName(REFGUID a_guid);

		HRESULT QueryInterface(HandleCore& a_core, IUnknown* a_self, std::span<const IID> a_iids, REFIID a_riid, void** a_out, void* a_caller);
		HRESULT SetPrivateData(HandleCore& a_core, std::uint16_t a_slot, REFGUID a_guid, UINT a_size, const void* a_data, void* a_caller);
		HRESULT SetPrivateDataInterface(HandleCore& a_core, std::uint16_t a_slot, REFGUID a_guid, const IUnknown* a_data, void* a_caller);
		HRESULT GetPrivateData(HandleCore& a_core, std::uint16_t a_slot, REFGUID a_guid, UINT* a_size, void* a_data, void* a_caller);
	}

	/** @brief IUnknown and the debug name for every handle class. Self is the final class and is deleted through it. */
	template <class Self, class Trips, Kind K>
	class Handle : public Trips
	{
	public:
		static constexpr Kind kKind = K;
		using typename Trips::Interface;
		using typename Trips::Slot;

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_out) override
		{
			return Detail::QueryInterface(this->core, static_cast<Interface*>(this), Trips::kIids, a_riid, a_out, _ReturnAddress());
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++this->core.refs; }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG left = --this->core.refs;
			if (left == 0)
				delete static_cast<Self*>(this);
			return left;
		}
		HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID a_guid, UINT a_size, const void* a_data) override
		{
			return Detail::SetPrivateData(this->core, static_cast<std::uint16_t>(Slot::SetPrivateData), a_guid, a_size, a_data, _ReturnAddress());
		}
		HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID a_guid, UINT* a_size, void* a_data) override
		{
			return Detail::GetPrivateData(this->core, static_cast<std::uint16_t>(Slot::GetPrivateData), a_guid, a_size, a_data, _ReturnAddress());
		}
		HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID a_guid, const IUnknown* a_data) override
		{
			return Detail::SetPrivateDataInterface(this->core, static_cast<std::uint16_t>(Slot::SetPrivateDataInterface), a_guid, a_data, _ReturnAddress());
		}

		static HandleCore* CoreOf(void* a_object) { return &static_cast<Self*>(static_cast<Interface*>(a_object))->core; }

		template <class... A>
		static winrt::com_ptr<Interface> Make(Options&& a_options, IUnknown* a_wrap, A&&... a_args)
		{
			static_assert(!std::is_abstract_v<Self>, "a vtable slot has no implementation");
			auto* self = new Self(std::forward<A>(a_args)...);
			self->core.Init(K, std::move(a_options), a_wrap);
			Interface* object = self;
			Detail::RegisterHandleClass(K, *reinterpret_cast<void* const*>(object), &CoreOf);
			winrt::com_ptr<Interface> out;
			out.attach(object);
			return out;
		}

	protected:
		Handle() = default;
	};

	template <class Trips, Kind K, class Desc, D3D11_RESOURCE_DIMENSION Dimension>
	class ResourceHandle final : public Handle<ResourceHandle<Trips, K, Desc, Dimension>, Trips, K>
	{
	public:
		explicit ResourceHandle(const Desc& a_desc) :
			descs{ a_desc, a_desc } {}
		void STDMETHODCALLTYPE GetDesc(Desc* a_out) override
		{
			if (a_out)
				*a_out = CurrentDesc();
		}
		void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* a_out) override
		{
			if (a_out)
				*a_out = Dimension;
		}

		Desc CurrentDesc() const { return descs[current.load(std::memory_order_acquire)]; }
		/** @brief Render owner only. Fills the spare copy, then flips to it. */
		void SetDesc(const Desc& a_desc)
		{
			const std::uint8_t next = current.load(std::memory_order_relaxed) ^ 1u;
			descs[next] = a_desc;
			current.store(next, std::memory_order_release);
		}

	private:
		std::array<Desc, 2> descs;
		std::atomic<std::uint8_t> current{ 0 };
	};

	/** @brief Views answer GetResource when made with their resource. */
	template <class Trips, Kind K, class Desc>
	class ViewHandle final : public Handle<ViewHandle<Trips, K, Desc>, Trips, K>
	{
	public:
		ViewHandle(ID3D11Resource* a_resource, const Desc& a_desc) :
			descs{ a_desc, a_desc }
		{
			resource.copy_from(a_resource);
		}
		void STDMETHODCALLTYPE GetDesc(Desc* a_out) override
		{
			if (a_out)
				*a_out = CurrentDesc();
		}
		void STDMETHODCALLTYPE GetResource(ID3D11Resource** a_out) override
		{
			if (resource) {
				if (a_out)
					resource.copy_to(a_out);
				return;
			}
			if (this->core.Enter(static_cast<std::uint16_t>(Trips::Slot::GetResource), "GetResource", _ReturnAddress())) {
				static_cast<typename Trips::Interface*>(this->core.Wrapped())->GetResource(a_out);
				return;
			}
			Detail::Zero(a_out);
		}

		Desc CurrentDesc() const { return descs[current.load(std::memory_order_acquire)]; }
		/** @brief Render owner only. Fills the spare copy, then flips to it. */
		void SetDesc(const Desc& a_desc)
		{
			const std::uint8_t next = current.load(std::memory_order_relaxed) ^ 1u;
			descs[next] = a_desc;
			current.store(next, std::memory_order_release);
		}

		/** @brief The texture handle GetResource answers. Retarget leaves it: it is the field's stable texture handle. */
		winrt::com_ptr<ID3D11Resource> resource;

	private:
		std::array<Desc, 2> descs;
		std::atomic<std::uint8_t> current{ 0 };
	};

	template <class Trips, Kind K, class Desc>
	class DescHandle final : public Handle<DescHandle<Trips, K, Desc>, Trips, K>
	{
	public:
		explicit DescHandle(const Desc& a_desc) :
			desc(a_desc) {}
		void STDMETHODCALLTYPE GetDesc(Desc* a_out) override
		{
			if (a_out)
				*a_out = desc;
		}

		const Desc desc;
	};

	template <class Trips, Kind K>
	class ShaderHandle final : public Handle<ShaderHandle<Trips, K>, Trips, K>
	{
	public:
		explicit ShaderHandle(std::span<const std::uint8_t> a_bytecode) :
			bytecode(a_bytecode.begin(), a_bytecode.end()) {}

		const std::vector<std::uint8_t> bytecode;
	};

	class InputLayoutHandle final : public Handle<InputLayoutHandle, ID3D11InputLayoutTrips, Kind::InputLayout>
	{
	public:
		InputLayoutHandle(std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements, std::span<const std::uint8_t> a_vsBytecode);

		std::vector<std::string> semantics;
		std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
		const std::vector<std::uint8_t> vsBytecode;
	};

	/** @brief The device. GetImmediateContext answers the paired context handle, so readers get the wrapping one. */
	class DeviceHandle final : public Handle<DeviceHandle, ID3D11DeviceTrips, Kind::Device>
	{
	public:
		~DeviceHandle();
		void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture2D** a_out) override;

		/** @name Answered by a native handle, forwarded by a guest one, tripwires otherwise. */
		/** @{ */
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Buffer** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture1D** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture3D** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource* a_resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* a_desc,
			ID3D11ShaderResourceView** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource* a_resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* a_desc,
			ID3D11UnorderedAccessView** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource* a_resource, const D3D11_RENDER_TARGET_VIEW_DESC* a_desc,
			ID3D11RenderTargetView** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource* a_resource, const D3D11_DEPTH_STENCIL_VIEW_DESC* a_desc,
			ID3D11DepthStencilView** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* a_elements, UINT a_count, const void* a_bytecode, SIZE_T a_length,
			ID3D11InputLayout** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* a_bytecode, SIZE_T a_length, ID3D11ClassLinkage* a_linkage, ID3D11VertexShader** a_out) override;
		HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* a_bytecode, SIZE_T a_length, ID3D11ClassLinkage* a_linkage, ID3D11PixelShader** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateComputeShader(const void* a_bytecode, SIZE_T a_length, ID3D11ClassLinkage* a_linkage, ID3D11ComputeShader** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC* a_desc, ID3D11BlendState** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* a_desc, ID3D11DepthStencilState** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC* a_desc, ID3D11RasterizerState** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC* a_desc, ID3D11SamplerState** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC* a_desc, ID3D11Query** a_out) override;
		D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override;
		UINT STDMETHODCALLTYPE GetCreationFlags() override;
		HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;
		HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE a_feature, void* a_data, UINT a_size) override;
		HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT a_format, UINT* a_support) override;
		/** @} */

		/** @brief The paired context, set once by PairDeviceAndContext. */
		winrt::com_ptr<ID3D11DeviceContext> immediate;
	};

	/**
	 * @brief The context, in either graph mode. It keeps every bind and answers its getters, and drops draws, dispatches and
	 * everything else it does not implement. A guest context forwards only Map, Unmap, UpdateSubresource, the copies, the clears,
	 * GenerateMips, Begin, End, GetData and Flush to DXVK; a native one answers those through the native services.
	 * GetDevice answers the paired device handle.
	 */
	class DeviceContextHandle final : public Handle<DeviceContextHandle, ID3D11DeviceContextTrips, Kind::DeviceContext>
	{
	public:
		void STDMETHODCALLTYPE GetDevice(ID3D11Device** a_out) override;

		/** @name Answered by a native handle, forwarded by a guest one, tripwires otherwise. */
		/** @{ */
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_out) override;
		HRESULT STDMETHODCALLTYPE Map(ID3D11Resource* a_resource, UINT a_subresource, D3D11_MAP a_type, UINT a_flags, D3D11_MAPPED_SUBRESOURCE* a_out) override;
		void STDMETHODCALLTYPE Unmap(ID3D11Resource* a_resource, UINT a_subresource) override;
		void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource* a_resource, UINT a_subresource, const D3D11_BOX* a_box, const void* a_data, UINT a_rowPitch,
			UINT a_depthPitch) override;
		void STDMETHODCALLTYPE CopyResource(ID3D11Resource* a_destination, ID3D11Resource* a_source) override;
		void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource* a_destination, UINT a_destinationSubresource, UINT a_x, UINT a_y, UINT a_z,
			ID3D11Resource* a_source, UINT a_sourceSubresource, const D3D11_BOX* a_box) override;
		void STDMETHODCALLTYPE GenerateMips(ID3D11ShaderResourceView* a_view) override;
		void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView* a_view, const FLOAT a_color[4]) override;
		void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* a_view, const UINT a_values[4]) override;
		void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* a_view, const FLOAT a_values[4]) override;
		void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil) override;
		void STDMETHODCALLTYPE Begin(ID3D11Asynchronous* a_query) override;
		void STDMETHODCALLTYPE End(ID3D11Asynchronous* a_query) override;
		HRESULT STDMETHODCALLTYPE GetData(ID3D11Asynchronous* a_query, void* a_data, UINT a_size, UINT a_flags) override;
		/** @name Binds. A setter stores its bind and a getter answers it, in both modes. Nothing draws with them. */
		/** @{ */
		void STDMETHODCALLTYPE VSSetShaderResources(UINT a_start, UINT a_count, ID3D11ShaderResourceView* const* a_items) override;
		void STDMETHODCALLTYPE VSGetShaderResources(UINT a_start, UINT a_count, ID3D11ShaderResourceView** a_out) override;
		void STDMETHODCALLTYPE PSSetShaderResources(UINT a_start, UINT a_count, ID3D11ShaderResourceView* const* a_items) override;
		void STDMETHODCALLTYPE PSGetShaderResources(UINT a_start, UINT a_count, ID3D11ShaderResourceView** a_out) override;
		void STDMETHODCALLTYPE CSSetShaderResources(UINT a_start, UINT a_count, ID3D11ShaderResourceView* const* a_items) override;
		void STDMETHODCALLTYPE CSGetShaderResources(UINT a_start, UINT a_count, ID3D11ShaderResourceView** a_out) override;
		void STDMETHODCALLTYPE VSSetConstantBuffers(UINT a_start, UINT a_count, ID3D11Buffer* const* a_items) override;
		void STDMETHODCALLTYPE VSGetConstantBuffers(UINT a_start, UINT a_count, ID3D11Buffer** a_out) override;
		void STDMETHODCALLTYPE PSSetConstantBuffers(UINT a_start, UINT a_count, ID3D11Buffer* const* a_items) override;
		void STDMETHODCALLTYPE PSGetConstantBuffers(UINT a_start, UINT a_count, ID3D11Buffer** a_out) override;
		void STDMETHODCALLTYPE CSSetConstantBuffers(UINT a_start, UINT a_count, ID3D11Buffer* const* a_items) override;
		void STDMETHODCALLTYPE CSGetConstantBuffers(UINT a_start, UINT a_count, ID3D11Buffer** a_out) override;
		void STDMETHODCALLTYPE VSSetSamplers(UINT a_start, UINT a_count, ID3D11SamplerState* const* a_items) override;
		void STDMETHODCALLTYPE VSGetSamplers(UINT a_start, UINT a_count, ID3D11SamplerState** a_out) override;
		void STDMETHODCALLTYPE PSSetSamplers(UINT a_start, UINT a_count, ID3D11SamplerState* const* a_items) override;
		void STDMETHODCALLTYPE PSGetSamplers(UINT a_start, UINT a_count, ID3D11SamplerState** a_out) override;
		void STDMETHODCALLTYPE CSSetSamplers(UINT a_start, UINT a_count, ID3D11SamplerState* const* a_items) override;
		void STDMETHODCALLTYPE CSGetSamplers(UINT a_start, UINT a_count, ID3D11SamplerState** a_out) override;
		void STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader* a_shader, ID3D11ClassInstance* const* a_instances, UINT a_instanceCount) override;
		void STDMETHODCALLTYPE VSGetShader(ID3D11VertexShader** a_out, ID3D11ClassInstance** a_instances, UINT* a_instanceCount) override;
		void STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader* a_shader, ID3D11ClassInstance* const* a_instances, UINT a_instanceCount) override;
		void STDMETHODCALLTYPE PSGetShader(ID3D11PixelShader** a_out, ID3D11ClassInstance** a_instances, UINT* a_instanceCount) override;
		void STDMETHODCALLTYPE CSSetShader(ID3D11ComputeShader* a_shader, ID3D11ClassInstance* const* a_instances, UINT a_instanceCount) override;
		void STDMETHODCALLTYPE CSGetShader(ID3D11ComputeShader** a_out, ID3D11ClassInstance** a_instances, UINT* a_instanceCount) override;
		// The engine binds no geometry, hull or domain shaders, but state backups save and restore them.
		void STDMETHODCALLTYPE GSSetShader(ID3D11GeometryShader* a_shader, ID3D11ClassInstance* const* a_instances, UINT a_instanceCount) override;
		void STDMETHODCALLTYPE GSGetShader(ID3D11GeometryShader** a_out, ID3D11ClassInstance** a_instances, UINT* a_instanceCount) override;
		void STDMETHODCALLTYPE HSSetShader(ID3D11HullShader* a_shader, ID3D11ClassInstance* const* a_instances, UINT a_instanceCount) override;
		void STDMETHODCALLTYPE HSGetShader(ID3D11HullShader** a_out, ID3D11ClassInstance** a_instances, UINT* a_instanceCount) override;
		void STDMETHODCALLTYPE DSSetShader(ID3D11DomainShader* a_shader, ID3D11ClassInstance* const* a_instances, UINT a_instanceCount) override;
		void STDMETHODCALLTYPE DSGetShader(ID3D11DomainShader** a_out, ID3D11ClassInstance** a_instances, UINT* a_instanceCount) override;
		void STDMETHODCALLTYPE CSSetUnorderedAccessViews(UINT a_start, UINT a_count, ID3D11UnorderedAccessView* const* a_views, const UINT* a_initialCounts) override;
		void STDMETHODCALLTYPE CSGetUnorderedAccessViews(UINT a_start, UINT a_count, ID3D11UnorderedAccessView** a_out) override;
		void STDMETHODCALLTYPE OMSetRenderTargets(UINT a_count, ID3D11RenderTargetView* const* a_views, ID3D11DepthStencilView* a_depth) override;
		void STDMETHODCALLTYPE OMGetRenderTargets(UINT a_count, ID3D11RenderTargetView** a_views, ID3D11DepthStencilView** a_depth) override;
		void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState* a_state, const FLOAT a_factor[4], UINT a_sampleMask) override;
		void STDMETHODCALLTYPE OMGetBlendState(ID3D11BlendState** a_state, FLOAT a_factor[4], UINT* a_sampleMask) override;
		void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState* a_state, UINT a_stencilRef) override;
		void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D11DepthStencilState** a_state, UINT* a_stencilRef) override;
		void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState* a_state) override;
		void STDMETHODCALLTYPE RSGetState(ID3D11RasterizerState** a_state) override;
		void STDMETHODCALLTYPE RSSetViewports(UINT a_count, const D3D11_VIEWPORT* a_viewports) override;
		void STDMETHODCALLTYPE RSGetViewports(UINT* a_count, D3D11_VIEWPORT* a_viewports) override;
		void STDMETHODCALLTYPE RSSetScissorRects(UINT a_count, const D3D11_RECT* a_rects) override;
		void STDMETHODCALLTYPE RSGetScissorRects(UINT* a_count, D3D11_RECT* a_rects) override;
		void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout* a_layout) override;
		void STDMETHODCALLTYPE IAGetInputLayout(ID3D11InputLayout** a_layout) override;
		void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY a_topology) override;
		void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* a_topology) override;
		void STDMETHODCALLTYPE IASetVertexBuffers(UINT a_start, UINT a_count, ID3D11Buffer* const* a_buffers, const UINT* a_strides, const UINT* a_offsets) override;
		void STDMETHODCALLTYPE IAGetVertexBuffers(UINT a_start, UINT a_count, ID3D11Buffer** a_buffers, UINT* a_strides, UINT* a_offsets) override;
		void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer* a_buffer, DXGI_FORMAT a_format, UINT a_offset) override;
		void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D11Buffer** a_buffer, DXGI_FORMAT* a_format, UINT* a_offset) override;
		void STDMETHODCALLTYPE ClearState() override;
		/** @} */
		/** @brief Succeeds silently in native mode, since every native operation is submitted when made. */
		void STDMETHODCALLTYPE Flush() override;
		/** @} */

		/** @brief The paired device without a reference, since the device holds this context. Cleared by the device's destructor. */
		std::atomic<ID3D11Device*> device{ nullptr };

		/** @brief What the engine bound, for readers that ask the context. Nothing draws with it. */
		struct Bindings
		{
			struct Stage
			{
				std::array<winrt::com_ptr<ID3D11ShaderResourceView>, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> srvs;
				std::array<winrt::com_ptr<ID3D11Buffer>, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> cbs;
				std::array<winrt::com_ptr<ID3D11SamplerState>, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers;
			};
			std::mutex lock;
			Stage vs, ps, cs;
			winrt::com_ptr<ID3D11VertexShader> vsShader;
			winrt::com_ptr<ID3D11PixelShader> psShader;
			winrt::com_ptr<ID3D11ComputeShader> csShader;
			winrt::com_ptr<ID3D11GeometryShader> gsShader;
			winrt::com_ptr<ID3D11HullShader> hsShader;
			winrt::com_ptr<ID3D11DomainShader> dsShader;
			std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, D3D11_1_UAV_SLOT_COUNT> csUavs;
			std::array<winrt::com_ptr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs;
			winrt::com_ptr<ID3D11DepthStencilView> dsv;
			winrt::com_ptr<ID3D11BlendState> blend;
			std::array<FLOAT, 4> blendFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
			UINT sampleMask = 0xffffffffu;
			winrt::com_ptr<ID3D11DepthStencilState> depthStencil;
			UINT stencilRef = 0;
			winrt::com_ptr<ID3D11RasterizerState> rasterizer;
			std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
			UINT viewportCount = 0;
			std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
			UINT scissorCount = 0;
			winrt::com_ptr<ID3D11InputLayout> inputLayout;
			D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
			std::array<winrt::com_ptr<ID3D11Buffer>, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertexBuffers;
			std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> strides{};
			std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> offsets{};
			winrt::com_ptr<ID3D11Buffer> indexBuffer;
			DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
			UINT indexOffset = 0;

			void Reset();
		};
		Bindings bindings;

	private:
		template <class T, std::size_t N>
		void Store(std::array<winrt::com_ptr<T>, N>& a_slots, UINT a_start, UINT a_count, T* const* a_items)
		{
			std::lock_guard guard(bindings.lock);
			for (UINT i = 0; i < a_count && a_start + i < N; ++i)
				a_slots[a_start + i].copy_from(a_items ? a_items[i] : nullptr);
		}

		template <class T, std::size_t N>
		void Load(const std::array<winrt::com_ptr<T>, N>& a_slots, UINT a_start, UINT a_count, T** a_out)
		{
			if (!a_out)
				return;
			std::lock_guard guard(bindings.lock);
			for (UINT i = 0; i < a_count; ++i) {
				a_out[i] = nullptr;
				if (a_start + i < N && a_slots[a_start + i])
					a_slots[a_start + i].copy_to(&a_out[i]);
			}
		}
	};

	/** @brief The swap chain. In native mode it goes to ORG's swap chain, and fullscreen calls succeed without changing anything. */
	class SwapChainHandle final : public Handle<SwapChainHandle, IDXGISwapChainTrips, Kind::SwapChain>
	{
	public:
		explicit SwapChainHandle(const DXGI_SWAP_CHAIN_DESC& a_desc) :
			desc(a_desc) {}
		HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* a_out) override;
		HRESULT STDMETHODCALLTYPE Present(UINT a_syncInterval, UINT a_flags) override;
		HRESULT STDMETHODCALLTYPE GetBuffer(UINT a_buffer, REFIID a_riid, void** a_out) override;
		HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT a_count, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags) override;
		HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* a_fullscreen, IDXGIOutput** a_target) override;
		HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL a_fullscreen, IDXGIOutput* a_target) override;
		HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* a_parameters) override;
		HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** a_out) override;

		const DXGI_SWAP_CHAIN_DESC desc;
	};

	using Texture1DHandle = ResourceHandle<ID3D11Texture1DTrips, Kind::Texture1D, D3D11_TEXTURE1D_DESC, D3D11_RESOURCE_DIMENSION_TEXTURE1D>;
	using Texture2DHandle = ResourceHandle<ID3D11Texture2DTrips, Kind::Texture2D, D3D11_TEXTURE2D_DESC, D3D11_RESOURCE_DIMENSION_TEXTURE2D>;
	using Texture3DHandle = ResourceHandle<ID3D11Texture3DTrips, Kind::Texture3D, D3D11_TEXTURE3D_DESC, D3D11_RESOURCE_DIMENSION_TEXTURE3D>;
	using BufferHandle = ResourceHandle<ID3D11BufferTrips, Kind::Buffer, D3D11_BUFFER_DESC, D3D11_RESOURCE_DIMENSION_BUFFER>;
	using ShaderResourceViewHandle = ViewHandle<ID3D11ShaderResourceViewTrips, Kind::ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>;
	using RenderTargetViewHandle = ViewHandle<ID3D11RenderTargetViewTrips, Kind::RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>;
	using DepthStencilViewHandle = ViewHandle<ID3D11DepthStencilViewTrips, Kind::DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>;
	using UnorderedAccessViewHandle = ViewHandle<ID3D11UnorderedAccessViewTrips, Kind::UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC>;
	using VertexShaderHandle = ShaderHandle<ID3D11VertexShaderTrips, Kind::VertexShader>;
	using PixelShaderHandle = ShaderHandle<ID3D11PixelShaderTrips, Kind::PixelShader>;
	using ComputeShaderHandle = ShaderHandle<ID3D11ComputeShaderTrips, Kind::ComputeShader>;
	using BlendStateHandle = DescHandle<ID3D11BlendStateTrips, Kind::BlendState, D3D11_BLEND_DESC>;
	using DepthStencilStateHandle = DescHandle<ID3D11DepthStencilStateTrips, Kind::DepthStencilState, D3D11_DEPTH_STENCIL_DESC>;
	using RasterizerStateHandle = DescHandle<ID3D11RasterizerStateTrips, Kind::RasterizerState, D3D11_RASTERIZER_DESC>;
	using SamplerStateHandle = DescHandle<ID3D11SamplerStateTrips, Kind::SamplerState, D3D11_SAMPLER_DESC>;
	using QueryHandle = DescHandle<ID3D11QueryTrips, Kind::Query, D3D11_QUERY_DESC>;
}

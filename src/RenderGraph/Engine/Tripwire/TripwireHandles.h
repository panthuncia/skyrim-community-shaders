#pragma once

// The tripwire handle classes (Tripwire.h). Only the tripwire's own sources include this; ports use Tripwire.h.

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
		/** @brief The guest context's filter (SetContextFilter), null when none. */
		extern std::atomic<ContextFilter> contextFilter;

		/** @brief The native services (SetNativeServices), null before they are installed. */
		extern std::atomic<const NativeServices*> nativeServices;
	}

	/** @brief The state every handle has, whatever its interface. */
	class HandleCore
	{
	public:
		HandleCore() = default;
		HandleCore(const HandleCore&) = delete;
		HandleCore& operator=(const HandleCore&) = delete;
		/** @brief Releases the wrapped object and queues the ORG resource and owner for ServiceRetired. */
		~HandleCore();

		/** @brief Set once by the factory, before the handle is handed out. a_wrap gets a reference of its own. */
		void Init(Kind a_kind, Options&& a_options, IUnknown* a_wrap);

		/**
		 * @brief The start of every tripwire method: records the call when it is watched (every call in native mode) and says whether
		 * it is forwarded (a wrapped object exists). A guest context asks its filter first (SetContextFilter): a dropped call is
		 * neither recorded nor forwarded.
		 */
		bool Enter(std::uint16_t a_slot, const char* a_method, void* a_caller)
		{
			if (kind == Kind::DeviceContext) {
				// The guest context's calls, and in native mode the native context's: there the filter decides which calls are the
				// latch's to drop (silently) and which the census counts.
				if (const auto filter = Detail::contextFilter.load(std::memory_order_relaxed);
					filter && (wrapped || Detail::nativeServices.load(std::memory_order_relaxed)) && !filter(a_slot, a_caller))
					return false;
			}
			const bool forward = wrapped != nullptr;
			if (!forward || watch.Test(a_slot))
				Record(a_slot, a_method, a_caller, forward, {});
			return forward;
		}

		/** @brief Counts a call at a_caller; the first one from there is logged. a_detail goes into that first line (an IID). */
		void Record(std::uint16_t a_slot, const char* a_method, void* a_caller, bool a_forwarded, std::string_view a_detail) const;

		/**
		 * @brief A forwarded call at a_caller passed the handle a_argument, unwrapped for the wrapped object: a reader that still goes
		 * through D3D11. Counted at the call's site like Record; the first one names the argument.
		 */
		void RecordArgument(std::uint16_t a_slot, const char* a_method, void* a_caller, const HandleCore& a_argument) const;

		std::string Name() const;
		void SetName(std::string a_name);

		Kind kind = Kind::Count;
		std::atomic<ULONG> refs{ 1 };
		IUnknown* wrapped = nullptr;
		WatchSet watch;
		std::shared_ptr<org::Resource> graph;
		std::shared_ptr<const void> owner;

		/**
		 * @brief A native handle's private-data interfaces (SetPrivateDataInterface): each held until the handle goes or the GUID is
		 * set again, as D3D11 holds them. AttachGraphResource's tag and the pipeline descriptions' tokens live here in native mode.
		 */
		std::vector<std::pair<GUID, winrt::com_ptr<IUnknown>>> privateInterfaces;
		std::mutex privateLock;

	private:
		mutable std::shared_mutex nameLock;
		std::string name;
	};

	namespace Detail
	{
		/** @brief A tripwire method's result when it fails: E_FAIL, nothing, or a zero value. */
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

		/** @brief Where a forwarded call came from: the handle forwarding it, its method and its caller. */
		struct CallSite
		{
			const HandleCore* core = nullptr;
			std::uint16_t slot = 0;
			const char* method = nullptr;
			void* caller = nullptr;
		};

		/** @brief A forwarded call passes the handle a_handle to the wrapped object: recorded at a_site (RecordArgument). */
		void NoteHandleArgument(const CallSite& a_site, const void* a_handle);

		/** @brief Unwrap for one argument of a forwarded call: a handle is recorded at a_site, then replaced by what it wraps. */
		template <class T>
		T* UnwrapAt(T* a_object, const CallSite& a_site)
		{
			if (!a_object || !IsHandle(a_object))
				return a_object;
			NoteHandleArgument(a_site, a_object);
			return Unwrap(a_object);
		}

		/**
		 * @brief An interface array with its handles replaced by the objects they wrap; the source itself when it holds none. With
		 * a_site, the first handle found is recorded there.
		 */
		template <class T>
		class UnwrappedArray
		{
		public:
			UnwrappedArray(T* const* a_source, UINT a_count, const CallSite* a_site = nullptr) :
				result(a_source)
			{
				if (!a_source)
					return;
				UINT first = a_count;
				for (UINT i = 0; i < a_count && first == a_count; ++i)
					if (a_source[i] && IsHandle(a_source[i]))
						first = i;
				if (first == a_count)
					return;
				if (a_site)
					NoteHandleArgument(*a_site, a_source[first]);
				T** out = local.data();
				if (a_count > local.size()) {
					heap.resize(a_count);
					out = heap.data();
				}
				for (UINT i = 0; i < a_count; ++i)
					out[i] = Unwrap(a_source[i]);
				result = out;
			}
			UnwrappedArray(const UnwrappedArray&) = delete;
			UnwrappedArray& operator=(const UnwrappedArray&) = delete;

			operator T* const*() const { return result; }

		private:
			T* const* result;
			std::array<T*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> local{};
			std::vector<T*> heap;
		};

		template <class T>
		UnwrappedArray<T> UnwrapArray(T* const* a_source, UINT a_count, const CallSite* a_site = nullptr)
		{
			return { a_source, a_count, a_site };
		}

		/** @brief Makes a handle class's objects recognisable (IsHandle): its vtable and how to reach its core. */
		void RegisterHandleClass(Kind a_kind, const void* a_vtable, HandleCore* (*a_core)(void*));

		/** @brief The core of a handle, or null when a_object is not one. */
		HandleCore* CoreOf(const void* a_object);

		/** @brief The self-test: the calls counted for a_kind's a_method (every site), and clearing every site afterwards. */
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
#define TW_UNWRAP_ARRAY(p, n) ::RenderGraph::Engine::Tripwire::Detail::UnwrapArray(p, n, &twSite)

#define TW_SLOT_ENUM(Ret, Name, Params, Args, Fail) Name,
#define TW_SLOT_NAME(Ret, Name, Params, Args, Fail) #Name,
// A tripwire method: recorded as HandleCore::Enter decides, forwarded when an object is wrapped, else failed. A forwarded
// call's handle arguments are recorded at its site (twSite) as they are unwrapped.
#define TW_TRIP(Ret, Name, Params, Args, Fail)                                                         \
	Ret STDMETHODCALLTYPE Name Params override                                                         \
	{                                                                                                  \
		void* const twCaller = _ReturnAddress();                                                       \
		if (core.Enter(static_cast<std::uint16_t>(Slot::Name), #Name, twCaller)) {                     \
			[[maybe_unused]] const ::RenderGraph::Engine::Tripwire::Detail::CallSite twSite{ &core,    \
				static_cast<std::uint16_t>(Slot::Name), #Name, twCaller };                             \
			return static_cast<Interface*>(core.wrapped)->Name Args;                                   \
		}                                                                                              \
		TW_STRIP Fail return ::RenderGraph::Engine::Tripwire::Detail::Failure<Ret>();                  \
	}

// Every method of Iface as a tripwire. The compiler checks the list against the SDK: each entry is an override, and a class
// that misses a slot stays abstract (the factories' static_asserts). The slot count matches the generator's.
#define TW_DECLARE_TRIPS(Iface)                                                                                       \
	class Iface##Trips : public Iface                                                                                 \
	{                                                                                                                 \
	public:                                                                                                           \
		using Interface = Iface;                                                                                      \
		enum class Slot : std::uint16_t { QueryInterface, AddRef, Release, TW_SLOTS_##Iface(TW_SLOT_ENUM) Count };   \
		static_assert(static_cast<std::uint16_t>(Slot::Count) == TW_SLOT_COUNT_##Iface);                             \
		static_assert(static_cast<std::size_t>(Slot::Count) <= ::RenderGraph::Engine::Tripwire::WatchSet::kMaxSlots); \
		static constexpr const char* kSlotNames[] = { "QueryInterface", "AddRef", "Release",                          \
			TW_SLOTS_##Iface(TW_SLOT_NAME) };                                                                         \
		static inline const IID kIids[] = { TW_IIDS_##Iface };                                                        \
		TW_SLOTS_##Iface(TW_TRIP)                                                                                     \
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
	TW_DECLARE_TRIPS(ID3D11DeviceContext)
	TW_DECLARE_TRIPS(ID3D11Device)
	TW_DECLARE_TRIPS(IDXGISwapChain)

	namespace Detail
	{
		/** @brief WKPDID_D3DDebugObjectName (Util::WKPDID_D3DDebugObjectNameT). */
		bool IsDebugName(REFGUID a_guid);

		HRESULT QueryInterface(HandleCore& a_core, IUnknown* a_self, std::span<const IID> a_iids, REFIID a_riid, void** a_out, void* a_caller);
		HRESULT SetPrivateData(HandleCore& a_core, std::uint16_t a_slot, REFGUID a_guid, UINT a_size, const void* a_data, void* a_caller);
		HRESULT SetPrivateDataInterface(HandleCore& a_core, std::uint16_t a_slot, REFGUID a_guid, const IUnknown* a_data, void* a_caller);
		HRESULT GetPrivateData(HandleCore& a_core, std::uint16_t a_slot, REFGUID a_guid, UINT* a_size, void* a_data, void* a_caller);
	}

	/**
	 * @brief IUnknown and the debug name for every handle class. Self is the final class (deleted through it), Trips its interface's
	 * tripwire base.
	 */
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

		/** @brief For Detail::RegisterHandleClass: the core of an object of this class, from any of its interface pointers. */
		static HandleCore* CoreOf(void* a_object) { return &static_cast<Self*>(static_cast<Interface*>(a_object))->core; }

		/** @brief Makes the handle and registers its class. */
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

	/** @brief Textures and buffers: GetDesc and GetType answered. */
	template <class Trips, Kind K, class Desc, D3D11_RESOURCE_DIMENSION Dimension>
	class ResourceHandle final : public Handle<ResourceHandle<Trips, K, Desc, Dimension>, Trips, K>
	{
	public:
		explicit ResourceHandle(const Desc& a_desc) :
			desc(a_desc) {}
		void STDMETHODCALLTYPE GetDesc(Desc* a_out) override
		{
			if (a_out)
				*a_out = desc;
		}
		void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* a_out) override
		{
			if (a_out)
				*a_out = Dimension;
		}

		const Desc desc;
	};

	/** @brief Views: GetDesc answered, and GetResource when the view was made with its resource. */
	template <class Trips, Kind K, class Desc>
	class ViewHandle final : public Handle<ViewHandle<Trips, K, Desc>, Trips, K>
	{
	public:
		ViewHandle(ID3D11Resource* a_resource, const Desc& a_desc) :
			desc(a_desc)
		{
			resource.copy_from(a_resource);
		}
		void STDMETHODCALLTYPE GetDesc(Desc* a_out) override
		{
			if (a_out)
				*a_out = desc;
		}
		void STDMETHODCALLTYPE GetResource(ID3D11Resource** a_out) override
		{
			if (resource) {
				if (a_out)
					resource.copy_to(a_out);
				return;
			}
			if (this->core.Enter(static_cast<std::uint16_t>(Trips::Slot::GetResource), "GetResource", _ReturnAddress())) {
				static_cast<typename Trips::Interface*>(this->core.wrapped)->GetResource(a_out);
				return;
			}
			Detail::Zero(a_out);
		}

		const Desc desc;
		winrt::com_ptr<ID3D11Resource> resource;
	};

	/** @brief State objects and queries: GetDesc answered. */
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

	/** @brief Shaders: their bytecode kept for the pipeline that replaces them. */
	template <class Trips, Kind K>
	class ShaderHandle final : public Handle<ShaderHandle<Trips, K>, Trips, K>
	{
	public:
		explicit ShaderHandle(std::span<const std::uint8_t> a_bytecode) :
			bytecode(a_bytecode.begin(), a_bytecode.end()) {}

		const std::vector<std::uint8_t> bytecode;
	};

	/** @brief Input layouts: the elements (with their semantic names) and the vertex shader signature they were made against. */
	class InputLayoutHandle final : public Handle<InputLayoutHandle, ID3D11InputLayoutTrips, Kind::InputLayout>
	{
	public:
		InputLayoutHandle(std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements, std::span<const std::uint8_t> a_vsBytecode);

		std::vector<std::string> semantics;
		std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
		const std::vector<std::uint8_t> vsBytecode;
	};

	/**
	 * @brief The device: GetImmediateContext answers the context handle paired with it (PairDeviceAndContext), so a reader that
	 * asks the device for its context gets the wrapping one. CreateTexture2D asks the calling thread's ScopedCreateTexture2DHook
	 * first. Everything else but IUnknown and the debug name is a tripwire.
	 */
	class DeviceHandle final : public Handle<DeviceHandle, ID3D11DeviceTrips, Kind::Device>
	{
	public:
		~DeviceHandle();
		void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext** a_out) override;
		HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture2D** a_out) override;

		/** @name Native mode (NativeServices): answered by a native handle, forwarded by a guest one, tripwires otherwise. */
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

		/** @brief The paired context (a reference held), set once by PairDeviceAndContext. */
		winrt::com_ptr<ID3D11DeviceContext> immediate;
	};

	/**
	 * @brief The context: GetDevice answers the device handle paired with it. Everything else but IUnknown and the debug name is a
	 * tripwire.
	 */
	class DeviceContextHandle final : public Handle<DeviceContextHandle, ID3D11DeviceContextTrips, Kind::DeviceContext>
	{
	public:
		void STDMETHODCALLTYPE GetDevice(ID3D11Device** a_out) override;

		/** @name Native mode (NativeServices): answered by a native handle, forwarded by a guest one (through its filter), tripwires otherwise. */
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
		/** @name Binds: in native mode a setter stores what it binds (unless the flush latch drops it) and a getter answers it. */
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
		/** @brief Native mode: nothing to send on (every native operation is submitted when it is made), so it succeeds silently. */
		void STDMETHODCALLTYPE Flush() override;
		/** @} */

		/** @brief The paired device, no reference held (the device holds this context): cleared by the device's destructor. */
		std::atomic<ID3D11Device*> device{ nullptr };

		/**
		 * @brief Native mode: what D3D11 code bound, for the readers that ask the context (DCLF's main-pass capture, the ports that
		 * save and restore state). Nothing draws with it: the ports read the engine's state from the shadow state and these.
		 */
		struct NativeBindings
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

			/** @brief ClearState: nothing bound, the default states. */
			void Reset();
		};
		NativeBindings bindings;

	private:
		/** @brief A native setter's bind is kept unless the flush latch drops it (the context filter, as for the guest context). */
		static bool NativeBindAllowed(std::uint16_t a_slot, void* a_caller)
		{
			const auto filter = Detail::contextFilter.load(std::memory_order_acquire);
			return !filter || filter(a_slot, a_caller);
		}

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

	/**
	 * @brief The swap chain: GetDesc answered. In native mode (NativeServices) Present, GetBuffer and ResizeBuffers go to ORG's swap
	 * chain, and the fullscreen calls succeed without changing anything (the window stays borderless, as fake fullscreen does).
	 */
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

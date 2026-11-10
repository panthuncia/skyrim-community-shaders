#pragma once

// The tripwire handle classes (Tripwire.h). Only the tripwire's own sources include this; ports use Tripwire.h.

#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Engine/Tripwire/TripwireInterfaces.inl"

#include <array>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <shared_mutex>
#include <type_traits>

namespace RenderGraph::Engine::Tripwire
{
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
		 * it is forwarded (a wrapped object exists).
		 */
		bool Enter(std::uint16_t a_slot, const char* a_method, void* a_caller)
		{
			const bool forward = wrapped != nullptr;
			if (!forward || watch.Test(a_slot))
				Record(a_slot, a_method, a_caller, forward, {});
			return forward;
		}

		/** @brief Counts a call at a_caller; the first one from there is logged. a_detail goes into that first line (an IID). */
		void Record(std::uint16_t a_slot, const char* a_method, void* a_caller, bool a_forwarded, std::string_view a_detail) const;

		std::string Name() const;
		void SetName(std::string a_name);

		Kind kind = Kind::Count;
		std::atomic<ULONG> refs{ 1 };
		IUnknown* wrapped = nullptr;
		WatchSet watch;
		std::shared_ptr<org::Resource> graph;
		std::shared_ptr<const void> owner;

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

		/** @brief An interface array with its handles replaced by the objects they wrap; the source itself when it holds none. */
		template <class T>
		class UnwrappedArray
		{
		public:
			UnwrappedArray(T* const* a_source, UINT a_count) :
				result(a_source)
			{
				if (!a_source)
					return;
				bool any = false;
				for (UINT i = 0; i < a_count && !any; ++i)
					any = a_source[i] && IsHandle(a_source[i]);
				if (!any)
					return;
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
		UnwrappedArray<T> UnwrapArray(T* const* a_source, UINT a_count)
		{
			return { a_source, a_count };
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
#define TW_UNWRAP(p) ::RenderGraph::Engine::Tripwire::Unwrap(p)
#define TW_UNWRAP_ARRAY(p, n) ::RenderGraph::Engine::Tripwire::Detail::UnwrapArray(p, n)

#define TW_SLOT_ENUM(Ret, Name, Params, Args, Fail) Name,
#define TW_SLOT_NAME(Ret, Name, Params, Args, Fail) #Name,
// A tripwire method: recorded as HandleCore::Enter decides, forwarded when an object is wrapped, else failed.
#define TW_TRIP(Ret, Name, Params, Args, Fail)                                                         \
	Ret STDMETHODCALLTYPE Name Params override                                                         \
	{                                                                                                  \
		if (core.Enter(static_cast<std::uint16_t>(Slot::Name), #Name, _ReturnAddress()))               \
			return static_cast<Interface*>(core.wrapped)->Name Args;                                   \
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

	/** @brief The device and the context: everything but IUnknown and the debug name is a tripwire. */
	template <class Trips, Kind K>
	class PlainHandle final : public Handle<PlainHandle<Trips, K>, Trips, K>
	{
	};

	/** @brief The swap chain: GetDesc answered. */
	class SwapChainHandle final : public Handle<SwapChainHandle, IDXGISwapChainTrips, Kind::SwapChain>
	{
	public:
		explicit SwapChainHandle(const DXGI_SWAP_CHAIN_DESC& a_desc) :
			desc(a_desc) {}
		HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* a_out) override
		{
			if (!a_out)
				return E_INVALIDARG;
			*a_out = desc;
			return S_OK;
		}

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
	using DeviceContextHandle = PlainHandle<ID3D11DeviceContextTrips, Kind::DeviceContext>;
	using DeviceHandle = PlainHandle<ID3D11DeviceTrips, Kind::Device>;
}

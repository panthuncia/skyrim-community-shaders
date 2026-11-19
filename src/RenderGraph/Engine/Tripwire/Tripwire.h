#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <winrt/base.h>

namespace org
{
	class Resource;
}

/**
 * @brief Tripwire handles are COM objects that stand in the engine's ID3D11* and IDXGISwapChain fields once ORG owns the resource.
 *
 * IUnknown, GetDesc and debug names work. Every other method is a tripwire that logs its call site once and fails harmlessly.
 * A guest handle wraps a D3D11 object and forwards every call to it, logging only watched methods.
 * Never pass a handle straight to DXVK's own device or context, since DXVK casts it to its own class and crashes. Use Unwrap.
 * The last Release frees the handle on any thread, but its ORG resource is queued for ServiceRetired so GPU memory outlives use.
 * CS_ORG_TRIPWIRE_LOG=1 logs call site reports, and CS_ORG_TRIPWIRE_SELFTEST=1 runs a self-test at startup.
 */
namespace RenderGraph::Engine::Tripwire
{
	/** @brief The interface a handle implements. */
	enum class Kind : std::uint8_t
	{
		Texture1D,
		Texture2D,
		Texture3D,
		Buffer,
		ShaderResourceView,
		RenderTargetView,
		DepthStencilView,
		UnorderedAccessView,
		VertexShader,
		PixelShader,
		ComputeShader,
		InputLayout,
		BlendState,
		DepthStencilState,
		RasterizerState,
		SamplerState,
		Query,
		DeviceContext,
		Device,
		SwapChain,
		Count
	};

	const char* InterfaceName(Kind a_kind);

	/** @brief The interface's method names in vtable order, so a name's index is its vtable slot. */
	std::span<const char* const> SlotNames(Kind a_kind);

	/** @brief The methods a guest handle logs while it forwards them, being calls that should not happen any more. */
	class WatchSet
	{
	public:
		/** @brief More than the largest vtable, ID3D11DeviceContext's 115 slots. */
		static constexpr std::size_t kMaxSlots = 128;

		WatchSet() = default;

		static WatchSet All();

		/** @brief The named methods of a_kind's interface. Unknown names are logged and ignored. The lookup is by string, so reuse the set. */
		static WatchSet Of(Kind a_kind, std::initializer_list<std::string_view> a_methods);

		bool Test(std::uint16_t a_slot) const { return a_slot < kMaxSlots && bits.test(a_slot); }

	private:
		std::bitset<kMaxSlots> bits;
	};

	struct Options
	{
		/** @brief The debug name shown in the log. SetPrivateData can change it later. */
		std::string name;
		/** @brief The ORG resource the handle stands for. Released through ServiceRetired after the last Release. */
		std::shared_ptr<org::Resource> graph;
		/** @brief Anything else the handle keeps alive, released with the graph resource. */
		std::shared_ptr<const void> owner;
		/** @brief The methods logged while forwarded. Guest mode only. */
		WatchSet watch;
	};

	/** @name Factories
	 * Each makes a handle with one reference. a_wrap is the D3D11 object for guest mode, or null for native mode. They never fail.
	 * @{ */
	winrt::com_ptr<ID3D11Texture1D> CreateTexture1D(const D3D11_TEXTURE1D_DESC& a_desc, Options a_options = {}, ID3D11Texture1D* a_wrap = nullptr);
	winrt::com_ptr<ID3D11Texture2D> CreateTexture2D(const D3D11_TEXTURE2D_DESC& a_desc, Options a_options = {}, ID3D11Texture2D* a_wrap = nullptr);
	winrt::com_ptr<ID3D11Texture3D> CreateTexture3D(const D3D11_TEXTURE3D_DESC& a_desc, Options a_options = {}, ID3D11Texture3D* a_wrap = nullptr);
	winrt::com_ptr<ID3D11Buffer> CreateBuffer(const D3D11_BUFFER_DESC& a_desc, Options a_options = {}, ID3D11Buffer* a_wrap = nullptr);
	winrt::com_ptr<ID3D11ShaderResourceView> CreateShaderResourceView(ID3D11Resource* a_resource, const D3D11_SHADER_RESOURCE_VIEW_DESC& a_desc,
		Options a_options = {}, ID3D11ShaderResourceView* a_wrap = nullptr);
	winrt::com_ptr<ID3D11RenderTargetView> CreateRenderTargetView(ID3D11Resource* a_resource, const D3D11_RENDER_TARGET_VIEW_DESC& a_desc,
		Options a_options = {}, ID3D11RenderTargetView* a_wrap = nullptr);
	winrt::com_ptr<ID3D11DepthStencilView> CreateDepthStencilView(ID3D11Resource* a_resource, const D3D11_DEPTH_STENCIL_VIEW_DESC& a_desc,
		Options a_options = {}, ID3D11DepthStencilView* a_wrap = nullptr);
	winrt::com_ptr<ID3D11UnorderedAccessView> CreateUnorderedAccessView(ID3D11Resource* a_resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC& a_desc,
		Options a_options = {}, ID3D11UnorderedAccessView* a_wrap = nullptr);
	/** @brief Shaders keep a copy of their bytecode for the pipeline that replaces them. */
	winrt::com_ptr<ID3D11VertexShader> CreateVertexShader(std::span<const std::uint8_t> a_bytecode, Options a_options = {}, ID3D11VertexShader* a_wrap = nullptr);
	winrt::com_ptr<ID3D11PixelShader> CreatePixelShader(std::span<const std::uint8_t> a_bytecode, Options a_options = {}, ID3D11PixelShader* a_wrap = nullptr);
	winrt::com_ptr<ID3D11ComputeShader> CreateComputeShader(std::span<const std::uint8_t> a_bytecode, Options a_options = {}, ID3D11ComputeShader* a_wrap = nullptr);
	/** @brief Keeps copies of the elements and of the vertex shader signature it was made against. */
	winrt::com_ptr<ID3D11InputLayout> CreateInputLayout(std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements, std::span<const std::uint8_t> a_vsBytecode,
		Options a_options = {}, ID3D11InputLayout* a_wrap = nullptr);
	winrt::com_ptr<ID3D11BlendState> CreateBlendState(const D3D11_BLEND_DESC& a_desc, Options a_options = {}, ID3D11BlendState* a_wrap = nullptr);
	winrt::com_ptr<ID3D11DepthStencilState> CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC& a_desc, Options a_options = {}, ID3D11DepthStencilState* a_wrap = nullptr);
	winrt::com_ptr<ID3D11RasterizerState> CreateRasterizerState(const D3D11_RASTERIZER_DESC& a_desc, Options a_options = {}, ID3D11RasterizerState* a_wrap = nullptr);
	winrt::com_ptr<ID3D11SamplerState> CreateSamplerState(const D3D11_SAMPLER_DESC& a_desc, Options a_options = {}, ID3D11SamplerState* a_wrap = nullptr);
	winrt::com_ptr<ID3D11Query> CreateQuery(const D3D11_QUERY_DESC& a_desc, Options a_options = {}, ID3D11Query* a_wrap = nullptr);
	/** @brief The immediate context, the device and the swap chain. Everything but IUnknown is a tripwire. */
	winrt::com_ptr<ID3D11DeviceContext> CreateDeviceContext(Options a_options = {}, ID3D11DeviceContext* a_wrap = nullptr);
	winrt::com_ptr<ID3D11Device> CreateDevice(Options a_options = {}, ID3D11Device* a_wrap = nullptr);
	winrt::com_ptr<IDXGISwapChain> CreateSwapChain(const DXGI_SWAP_CHAIN_DESC& a_desc, Options a_options = {}, IDXGISwapChain* a_wrap = nullptr);
	/** @} */

	/** @brief Pairs a device handle with a context handle so each answers the other. Call once, before either is handed out. */
	bool PairDeviceAndContext(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	struct Info
	{
		Kind kind = Kind::Count;
		std::string name;
		std::shared_ptr<org::Resource> graph;
		/** @brief The wrapped D3D11 object without a reference added. Null in native mode. */
		IUnknown* wrapped = nullptr;
	};

	/** @brief Whether a_object is a tripwire handle. Safe on any COM object. */
	bool IsHandle(const void* a_object);

	/** @brief a_object's handle data. kind is Count when it is not a handle. */
	Info Describe(const void* a_object);

	/** @brief A handle's wrapped D3D11 object, cheaper than Describe. Null when there is none. */
	IUnknown* WrappedOf(const void* a_object);

	/** @brief A handle's kind, cheaper than Describe. Count when a_object is not a handle. */
	Kind KindOf(const void* a_object);

	/** @brief The ORG resource behind an engine field, whether it holds a handle or a plain object with AttachGraphResource. Any thread. */
	std::shared_ptr<org::Resource> GraphResourceOf(const void* a_object);

	/**
	 * @brief Ties an ORG resource to a plain DXVK object through private data, so GraphResourceOf finds it. Released through
	 * ServiceRetired when the object dies. Call once at creation, before the object is shared.
	 */
	bool AttachGraphResource(void* a_object, std::shared_ptr<org::Resource> a_graph, std::shared_ptr<const void> a_owner = {});

	/** @brief A shader handle's bytecode, which lives as long as the handle. Empty when not a shader handle. */
	std::span<const std::uint8_t> ShaderBytecode(const void* a_object);

	/** @brief An input layout handle's elements, with semantic names owned by the handle. */
	std::span<const D3D11_INPUT_ELEMENT_DESC> InputElements(const void* a_object);

	/** @brief The object a handle wraps without a reference. a_object itself when not a handle, null for a native handle. */
	IUnknown* UnwrapObject(IUnknown* a_object);

	template <class T>
	T* Unwrap(T* a_object)
	{
		if (!a_object || !IsHandle(a_object))
			return a_object;
		return static_cast<T*>(UnwrapObject(const_cast<IUnknown*>(static_cast<const IUnknown*>(a_object))));
	}

	/** @brief Makes a texture in place of a device handle's CreateTexture2D. False lets the call go on to the wrapped device. */
	using CreateTexture2DHook = bool (*)(const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture2D** a_out);

	/**
	 * @brief While alive, a device handle's CreateTexture2D on this thread asks a_hook first. No vtable is patched.
	 * Scopes nest, and the hook runs with none set so the textures it makes are plain.
	 */
	class ScopedCreateTexture2DHook
	{
	public:
		explicit ScopedCreateTexture2DHook(CreateTexture2DHook a_hook);
		~ScopedCreateTexture2DHook();
		ScopedCreateTexture2DHook(const ScopedCreateTexture2DHook&) = delete;
		ScopedCreateTexture2DHook& operator=(const ScopedCreateTexture2DHook&) = delete;

	private:
		CreateTexture2DHook previous;
	};

	/** @brief Hands resources of released handles to the runtime, which keeps them until the GPU is past the D3D11 work. Render thread. */
	void ServiceRetired();

	/** @brief Takes the released handles' resources instead of ServiceRetired. */
	std::vector<std::shared_ptr<const void>> TakeRetired();

	/** @brief Runs the self-test and starts the periodic report when their switches are on. Installs nothing in the engine. */
	void Start();

	/** @brief Logs every call site with its count when CS_ORG_TRIPWIRE_LOG=1. */
	void Report(std::string_view a_reason);

	/** @brief Stops the periodic report's thread and logs the exit report. */
	void Stop();

	/** @brief Meters the handle methods called on this thread for Tracy and the log, when CS_ORG_D3D11_METER=1. Render thread. */
	void MeterFrame();

	/** @brief Names a code address for logs, with the Address Library ID and offset for game code. */
	std::string DescribeCaller(const void* a_address);

	/** @brief Whether a_address lies in CS's own image. */
	bool IsOwnCode(const void* a_address);

	bool IsGameCode(const void* a_address);

	/** @brief The Address Library ID of the game function holding a_address and the offset in it. Any thread. */
	bool GameFunction(const void* a_address, std::uint64_t& a_id, std::uint64_t& a_within);

	/** @brief The owner a handle keeps, or null. Any thread. */
	std::shared_ptr<const void> OwnerOf(const void* a_object);

	/**
	 * @brief What native device and swap chain handles answer instead of failing, since there is no DXVK to forward to.
	 * A null entry leaves its method a tripwire. Guest handles never use these.
	 */
	struct NativeServices
	{
		/** @name Resources. Each returns S_OK with one reference in the last argument, or a failure with it null. */
		/** @{ */
		HRESULT (*createTexture1D)(const D3D11_TEXTURE1D_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture1D** a_out) = nullptr;
		HRESULT (*createTexture2D)(const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture2D** a_out) = nullptr;
		HRESULT (*createTexture3D)(const D3D11_TEXTURE3D_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture3D** a_out) = nullptr;
		HRESULT (*createBuffer)(const D3D11_BUFFER_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Buffer** a_out) = nullptr;
		/** @} */
		/** @brief The device's QueryInterface for IDXGIDevice1, which the engine sets frame latency through. */
		HRESULT (*queryDxgiDevice)(REFIID a_riid, void** a_out) = nullptr;
		/** @name The swap chain. */
		/** @{ */
		HRESULT (*present)(UINT a_syncInterval, UINT a_flags) = nullptr;
		HRESULT (*getBuffer)(UINT a_buffer, REFIID a_riid, void** a_out) = nullptr;
		HRESULT (*resizeBuffers)(UINT a_count, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags) = nullptr;
		HRESULT (*getDesc)(DXGI_SWAP_CHAIN_DESC* a_out) = nullptr;
		HRESULT (*getContainingOutput)(IDXGIOutput** a_out) = nullptr;
		/** @} */
		/** @name The device's queries. */
		/** @{ */
		HRESULT (*checkFeatureSupport)(D3D11_FEATURE a_feature, void* a_data, UINT a_size) = nullptr;
		HRESULT (*checkFormatSupport)(DXGI_FORMAT a_format, UINT* a_support) = nullptr;
		/** @} */
		/**
		 * @name The immediate context's work outside draws. Each runs as its own command work on the graphics queue, in call order,
		 * between the graph's epochs.
		 */
		/** @{ */
		HRESULT (*map)(ID3D11Resource* a_resource, UINT a_subresource, D3D11_MAP a_type, UINT a_flags, D3D11_MAPPED_SUBRESOURCE* a_out) = nullptr;
		void (*unmap)(ID3D11Resource* a_resource, UINT a_subresource) = nullptr;
		void (*updateSubresource)(ID3D11Resource* a_resource, UINT a_subresource, const D3D11_BOX* a_box, const void* a_data, UINT a_rowPitch,
			UINT a_depthPitch) = nullptr;
		void (*copyResource)(ID3D11Resource* a_destination, ID3D11Resource* a_source) = nullptr;
		void (*copySubresourceRegion)(ID3D11Resource* a_destination, UINT a_destinationSubresource, UINT a_x, UINT a_y, UINT a_z, ID3D11Resource* a_source,
			UINT a_sourceSubresource, const D3D11_BOX* a_box) = nullptr;
		void (*generateMips)(ID3D11ShaderResourceView* a_view) = nullptr;
		void (*clearRenderTargetView)(ID3D11RenderTargetView* a_view, const FLOAT a_color[4]) = nullptr;
		void (*clearUnorderedAccessViewUint)(ID3D11UnorderedAccessView* a_view, const UINT a_values[4]) = nullptr;
		void (*clearUnorderedAccessViewFloat)(ID3D11UnorderedAccessView* a_view, const FLOAT a_values[4]) = nullptr;
		void (*clearDepthStencilView)(ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil) = nullptr;
		/** @brief End marks the context's work so far, and GetData answers once it is done. */
		void (*end)(ID3D11Asynchronous* a_query) = nullptr;
		HRESULT (*getData)(ID3D11Asynchronous* a_query, void* a_data, UINT a_size, UINT a_flags) = nullptr;
		HRESULT (*queryContext)(REFIID a_riid, void** a_out) = nullptr;
		/** @} */
	};

	/** @brief Installs the native services once, before the native handles reach the engine. */
	void SetNativeServices(const NativeServices& a_services);

	bool NativeServicesInstalled();

	/**
	 * @brief Asked before the guest context forwards any call. False drops it as a native handle would.
	 * Must be cheap and must not call the guest context. Any thread.
	 */
	using ContextFilter = bool (*)(std::uint16_t a_slot, void* a_caller);

	/** @brief Installs the guest context's single filter, used by the residue census and the flush latch. Null removes it. */
	void SetContextFilter(ContextFilter a_filter);

	/**
	 * @brief Lets the flush latch take a guest Map or Unmap so nothing reaches DXVK, for engine constant buffers in a CS stage's
	 * window. Must not call the guest context. Render owner.
	 */
	struct GuestMapHooks
	{
		bool (*map)(ID3D11Resource* a_resource, UINT a_subresource, D3D11_MAP a_type, UINT a_flags, D3D11_MAPPED_SUBRESOURCE* a_out) = nullptr;
		bool (*unmap)(ID3D11Resource* a_resource, UINT a_subresource) = nullptr;
	};

	void SetGuestMapHooks(const GuestMapHooks& a_hooks);

	/** @brief Lets the skeleton's kMAIN copy run on the graph instead of DXVK. Must not call the guest context. Any thread. */
	using GuestCopyHook = bool (*)(ID3D11Resource* a_destination, ID3D11Resource* a_source);

	void SetGuestCopyHook(GuestCopyHook a_hook);
}

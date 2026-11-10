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
 * @brief Tripwire handles (org-full-port.md P4.1): COM objects that stand in the engine's ID3D11* and IDXGISwapChain fields once
 * ORG owns the resource behind them.
 *
 * A handle implements the whole vtable of its interface. IUnknown works (the refcount is atomic, Release from any thread),
 * QueryInterface answers only the handle's own interface chain, and GetDesc returns the description the handle was created with.
 * Debug names (SetPrivateData with WKPDID_D3DDebugObjectName) are kept. Every other method is a tripwire: it logs its caller once
 * per call site (module+offset, with the Address Library ID when the caller is the game), counts the calls, and fails (E_FAIL,
 * null or zeroed outputs, zero) without touching anything.
 *
 * Two modes, chosen per handle at creation:
 * - Native (nothing wrapped): every tripwire method fails as above.
 * - Guest (a D3D11 object wrapped, while DXVK is loaded): every method is forwarded to the wrapped object, so readers not ported
 *   yet keep working. Only the methods in the handle's WatchSet are logged. Arguments that are tripwire handles are replaced by
 *   the objects they wrap before forwarding, so a handle can be passed to a wrapped device or context. Objects a forwarded
 *   method returns (GetResource without a resource handle, the Get* methods of a context) are the wrapped D3D11 objects, not
 *   handles.
 *
 * A handle passed straight to DXVK's own device or context (not through a wrapping handle) crashes in DXVK, which casts it to its
 * own class. A port installs a handle only where every reader goes through its methods or through a wrapping device/context.
 *
 * Releases: the last Release frees the handle on the calling thread and releases the wrapped object (D3D11 objects are free
 * threaded). The ORG resource and the owner it keeps go to a queue that the render thread drains with ServiceRetired, so their
 * GPU memory outlives the frames that used them.
 *
 * Diagnostics: CS_ORG_TRIPWIRE_LOG=1 logs a report of every call site with its count, every CS_ORG_TRIPWIRE_LOG_INTERVAL seconds
 * (default 60, 0 = only at exit) when something changed, and at exit. CS_ORG_TRIPWIRE_SELFTEST=1 runs a self-test at startup
 * (TripwireSelfTest.cpp) that logs PASS or FAIL.
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

	/** @brief The interface's name ("ID3D11Texture2D"). */
	const char* InterfaceName(Kind a_kind);

	/** @brief The interface's method names in vtable order (QueryInterface, AddRef, Release first): a slot's index is its vtable slot. */
	std::span<const char* const> SlotNames(Kind a_kind);

	/** @brief The methods a guest handle logs while it forwards them (the calls that should not happen any more). */
	class WatchSet
	{
	public:
		/** @brief More than the largest vtable (ID3D11DeviceContext, 115 slots). */
		static constexpr std::size_t kMaxSlots = 128;

		/** @brief Nothing watched. */
		WatchSet() = default;

		/** @brief Every method of every interface. */
		static WatchSet All();

		/**
		 * @brief The named methods of a_kind's interface ("Map", "GetDevice"). A name the interface does not have is logged and
		 * ignored. Resolve a set once and reuse it: the lookup is by string.
		 */
		static WatchSet Of(Kind a_kind, std::initializer_list<std::string_view> a_methods);

		/** @brief Whether the method at vtable slot a_slot is watched. */
		bool Test(std::uint16_t a_slot) const { return a_slot < kMaxSlots && bits.test(a_slot); }

	private:
		std::bitset<kMaxSlots> bits;
	};

	/** @brief What every factory takes beside the description. */
	struct Options
	{
		/** @brief The debug name ("Engine::kMAIN"), shown in the log; SetPrivateData can change it later. */
		std::string name;
		/** @brief The ORG resource the handle stands for, if any. Released through ServiceRetired after the last Release. */
		std::shared_ptr<org::Resource> graph;
		/** @brief Anything else the handle keeps alive (an allocation, a record), released with the graph resource. */
		std::shared_ptr<const void> owner;
		/** @brief Guest mode: the methods logged while forwarded. Ignored in native mode, where every tripwire method logs. */
		WatchSet watch;
	};

	/** @name Factories
	 * Each makes a handle with one reference, from the description the engine would have passed to the device. a_wrap is the D3D11
	 * object for guest mode (the handle takes its own reference), or null for native mode. They never fail. Views take the
	 * resource they view (a handle or a D3D11 object, or null) and answer GetResource with it.
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
	/** @brief Shaders keep a copy of their bytecode (ShaderBytecode), for the pipeline that replaces them. */
	winrt::com_ptr<ID3D11VertexShader> CreateVertexShader(std::span<const std::uint8_t> a_bytecode, Options a_options = {}, ID3D11VertexShader* a_wrap = nullptr);
	winrt::com_ptr<ID3D11PixelShader> CreatePixelShader(std::span<const std::uint8_t> a_bytecode, Options a_options = {}, ID3D11PixelShader* a_wrap = nullptr);
	winrt::com_ptr<ID3D11ComputeShader> CreateComputeShader(std::span<const std::uint8_t> a_bytecode, Options a_options = {}, ID3D11ComputeShader* a_wrap = nullptr);
	/** @brief Keeps a copy of the elements (semantic names included) and of the vertex shader signature it was made against. */
	winrt::com_ptr<ID3D11InputLayout> CreateInputLayout(std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements, std::span<const std::uint8_t> a_vsBytecode,
		Options a_options = {}, ID3D11InputLayout* a_wrap = nullptr);
	winrt::com_ptr<ID3D11BlendState> CreateBlendState(const D3D11_BLEND_DESC& a_desc, Options a_options = {}, ID3D11BlendState* a_wrap = nullptr);
	winrt::com_ptr<ID3D11DepthStencilState> CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC& a_desc, Options a_options = {}, ID3D11DepthStencilState* a_wrap = nullptr);
	winrt::com_ptr<ID3D11RasterizerState> CreateRasterizerState(const D3D11_RASTERIZER_DESC& a_desc, Options a_options = {}, ID3D11RasterizerState* a_wrap = nullptr);
	winrt::com_ptr<ID3D11SamplerState> CreateSamplerState(const D3D11_SAMPLER_DESC& a_desc, Options a_options = {}, ID3D11SamplerState* a_wrap = nullptr);
	winrt::com_ptr<ID3D11Query> CreateQuery(const D3D11_QUERY_DESC& a_desc, Options a_options = {}, ID3D11Query* a_wrap = nullptr);
	/** @brief The immediate context, the device and the swap chain (P6.1): everything but IUnknown is a tripwire (the swap chain answers GetDesc). */
	winrt::com_ptr<ID3D11DeviceContext> CreateDeviceContext(Options a_options = {}, ID3D11DeviceContext* a_wrap = nullptr);
	winrt::com_ptr<ID3D11Device> CreateDevice(Options a_options = {}, ID3D11Device* a_wrap = nullptr);
	winrt::com_ptr<IDXGISwapChain> CreateSwapChain(const DXGI_SWAP_CHAIN_DESC& a_desc, Options a_options = {}, IDXGISwapChain* a_wrap = nullptr);
	/** @} */

	/** @brief What a handle holds, for the code that replaces its readers. */
	struct Info
	{
		Kind kind = Kind::Count;
		std::string name;
		std::shared_ptr<org::Resource> graph;
		/** @brief The wrapped D3D11 object (no reference added), null in native mode. */
		IUnknown* wrapped = nullptr;
	};

	/** @brief Whether a_object is a tripwire handle (any interface pointer of one). Safe on any COM object. */
	bool IsHandle(const void* a_object);

	/** @brief a_object's handle data, or an Info with kind Count when it is not a handle. */
	Info Describe(const void* a_object);

	/** @brief A shader handle's bytecode (empty when a_object is not a shader handle). Lives as long as the handle. */
	std::span<const std::uint8_t> ShaderBytecode(const void* a_object);

	/** @brief An input layout handle's elements, their semantic names owned by the handle (empty when not one). */
	std::span<const D3D11_INPUT_ELEMENT_DESC> InputElements(const void* a_object);

	/** @brief The object a handle wraps (no reference added): a_object itself when it is not a handle, null for a native handle. */
	IUnknown* UnwrapObject(IUnknown* a_object);

	/** @brief Typed UnwrapObject, for passing a handle to D3D11. */
	template <class T>
	T* Unwrap(T* a_object)
	{
		if (!a_object || !IsHandle(a_object))
			return a_object;
		return static_cast<T*>(UnwrapObject(const_cast<IUnknown*>(static_cast<const IUnknown*>(a_object))));
	}

	/**
	 * @brief Render thread, each frame (P4.2 installs the call): hands the ORG resources and owners of handles released since the
	 * last call to RenderGraphRuntime::ReleaseAfterD3D11, which keeps them until the GPU is past the D3D11 work issued so far. Without
	 * the render graph they are dropped.
	 */
	void ServiceRetired();

	/** @brief Takes the released handles' resources and owners instead of ServiceRetired (the self-test, a native frame's own retire). */
	std::vector<std::shared_ptr<const void>> TakeRetired();

	/**
	 * @brief Startup (kPostPostLoad): runs the self-test when CS_ORG_TRIPWIRE_SELFTEST=1 and starts the periodic report when
	 * CS_ORG_TRIPWIRE_LOG=1. Installs nothing in the engine.
	 */
	void Start();

	/** @brief CS_ORG_TRIPWIRE_LOG=1: logs every call site with its count now (at exit: a_reason "exit"). Otherwise does nothing. */
	void Report(std::string_view a_reason);

	/** @brief At the game window's close: stops the periodic report's thread, joins it, and logs the exit report. */
	void Stop();

	/**
	 * @brief How a code address is named in tripwire logs: "SkyrimSE.exe+0xd6a8f2 (ID 75467+0x82)" for the game (the Address Library
	 * ID of the function that contains it), "Module.dll+0x1234" for any other module, the bare address when no module holds it.
	 */
	std::string DescribeCaller(const void* a_address);
}

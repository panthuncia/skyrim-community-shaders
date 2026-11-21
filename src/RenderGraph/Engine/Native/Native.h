#pragma once

#include "RenderGraph/Engine/Engine.h"

#include <d3d11.h>
#include <dxgi.h>

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include <winrt/base.h>

struct DxvkOrgInteropDeviceInfo;
struct DxvkOrgInteropResourceInfo;
struct VkQueue_T;
struct DxvkOrgInteropTextureWrap;

namespace rhi
{
	class CommandList;
	class DevicePtr;
	class Resource;
	struct TimelinePoint;
	struct QueueProgress;
}

namespace org
{
	class ExternalTextureResource;
	class Runtime;
	class VersionedTexture;
}

/**
 * @brief Native mode (CS_ORG_NATIVE=1) renders without DXVK. CS makes a BasicRHI device itself, and the engine's device,
 * context and swap chain are tripwire handles over it. The render graph submits straight to the device's queues and ORG presents.
 * It is a development option. The DXVK build stays the default.
 *
 * Startup runs in numbered stages, and each logs one "[ORG native] stage" line so a failed start shows how far it got.
 * Anything that still needs DXVK fails at first use with a message naming it. Native mode never falls back to DXVK.
 */
namespace RenderGraph::Engine::Native
{
	/** @brief Whether CS_ORG_NATIVE=1 asks for native mode. Any thread. */
	bool Requested();

	/** @brief Whether native mode made its device. Any thread. */
	bool Active();

	/** @brief Logs one startup stage line. */
	void Stage(std::string_view a_stage, std::string_view a_detail = {});

	/** @brief Logs a startup error naming what still needs DXVK, then ends the process with a message box. */
	[[noreturn]] void Fail(std::string_view a_dependency, std::string_view a_why);

	/** @brief Stage 1. Fails on a switch that conflicts with native mode. Must run before any other switch is read. */
	void ApplySwitches();

	/**
	 * @brief In native or guest mode, logs why Drawcall Limit Fix will not run, then ends the process with a message box. Outside
	 * them it returns. Both modes withhold the engine's draws (the flush latch) and DCLF draws the world, so without it the world
	 * would be empty.
	 */
	void RequireDclf(std::string_view a_cause);

	/** @brief Stage 2. Fails on anything that still needs DXVK. Needs the features' boot state. */
	void CheckStartup();

	/** @brief CS's runtime DLL directory, CommunityShaders/bin. */
	std::filesystem::path RuntimeDirectory();

	/** @brief Stage 3, in place of D3D11CreateDeviceAndSwapChain. Returns tripwire handles over BasicRHI's device and ORG's swap chain. */
	HRESULT CreateDeviceAndSwapChain(IDXGIAdapter* a_adapter, DXGI_SWAP_CHAIN_DESC* a_desc, IDXGISwapChain** a_swapChain, ID3D11Device** a_device,
		D3D_FEATURE_LEVEL* a_featureLevel, ID3D11DeviceContext** a_context);

	/** @brief Checks that the engine's fields hold the native handles after its own InitD3D. */
	void AfterInitD3D();

	/** @brief BasicRHI's device once stage 3 made it, else null. */
	std::shared_ptr<rhi::DevicePtr> Device();

	/**
	 * @brief ORG's runtime, made with the device at stage 3 for the external client. RenderGraphRuntime::Initialize takes it once;
	 * null after that and off native mode.
	 */
	std::shared_ptr<org::Runtime> TakeOrgRuntime();

	/**
	 * @brief kFRAMEBUFFER's slot, made with the swap chain at stage 3. Every present copies its current version through the ledger.
	 * Native makes its versions (InitD3D's GetBuffer, a resize before Engine::Setup) until the engine target set adopts it. Null off
	 * native mode. Render thread.
	 */
	std::shared_ptr<org::VersionedTexture> BackBufferSlot();

	/**
	 * @brief The engine target set adopted BackBufferSlot (phase 3 section 3.7): its kFRAMEBUFFER twin's versions join it. From then on
	 * GetBuffer answers a_field and a swap chain resize makes no version. A different slot is fatal. Render thread, once, at
	 * Engine::Setup.
	 */
	void AdoptBackBufferSlot(const std::shared_ptr<org::VersionedTexture>& a_slot, winrt::com_ptr<ID3D11Texture2D> a_field);

	/** @brief The format the set declares kFRAMEBUFFER in: the swap chain's format without sRGB. UNKNOWN off native mode. Any thread after stage 3. */
	DXGI_FORMAT DeclaredBackBufferFormat();

	/**
	 * @brief The size the engine's last swap chain resize asked for, once the set owns the back buffer. False before that. The
	 * engine reads its size from GetBuffer's description, which is kFRAMEBUFFER's adopted one, so the set writes this into the
	 * engine's size state at a later set. Any thread.
	 */
	bool EngineSwapChainExtent(std::uint32_t& a_width, std::uint32_t& a_height);

	/** @brief Loads GenerateMips' compute programs. Must run after the shader compiler exists, or engine mip builds do nothing. */
	void OnShaderCompilerReady();

	/** @brief The native equivalent of dxvkGetInteropResourceInfo, for textures native mode made. */
	bool DescribeResource(const IUnknown* a_object, DxvkOrgInteropResourceInfo& a_info);

	/**
	 * @name Graph-owned textures. Shader-only textures made with contents are "exempt" and rest in the shader-read state untracked,
	 * until a context write promotes them. Every other texture is tracked as a persistent ORG texture once the graph is up.
	 * Each image's graph resource is found through ImageRegistry: a tracked texture's is registered as Owned when made, an exempt
	 * one's as Made on its first ImmutableTextureOf.
	 */
	/** @{ */
	/** @brief Whether native mode is active and sorts its textures as above. Any thread. */
	bool OwnedResources();

	/** @brief An exempt texture as a read-only graph resource, made on first call. Null when the texture is not exempt. Any thread. */
	std::shared_ptr<org::ExternalTextureResource> ImmutableTextureOf(const IUnknown* a_texture, DXGI_FORMAT a_srvFormat);

	/** @brief The graph resource of an exempt texture the context wrote after creation. Null for any other texture. Any thread. */
	std::shared_ptr<org::ExternalTextureResource> PromotedTextureOf(const IUnknown* a_object);

	/** @brief Whether a native texture must be tracked when graphs read it. False for exempt textures, promoted or not. Any thread. */
	bool IsTrackedTexture(const IUnknown* a_object);
	/** @} */

	struct ResourceFacts
	{
		std::uint64_t serial = 0;     // unique for the session
		std::uint32_t bindFlags = 0;  // D3D11_BIND_*
		std::uint32_t usage = 0;      // D3D11_USAGE
		std::uint32_t dimension = 0;  // D3D11_RESOURCE_DIMENSION
		bool initialData = false;     // made with initial data
		bool written = false;         // written by the immediate context since it was made
	};

	/** @brief The facts of the native resource behind a_object. False when native mode did not make it. Any thread. */
	bool DescribeFacts(const void* a_object, ResourceFacts& a_out);

	/** @brief The engine's Present. Copies the back buffer into the swap chain image after the frame's graph work. */
	HRESULT Present(UINT a_syncInterval, UINT a_flags);


	/** @brief The native graphics queue's progress. Empty off native mode. */
	rhi::QueueProgress GraphicsProgress();

	/** @name What DXVK's interop device exports give the profilers in guest mode. */
	/** @{ */
	/** @brief The native Vulkan device as DXVK describes its own, without extension or feature lists. False on D3D12. */
	bool DescribeVulkanDevice(DxvkOrgInteropDeviceInfo& a_info);

	/** @brief The adapter LUID as one 64-bit pattern, or 0. */
	std::uint64_t AdapterLuid();

	/** @brief What vendor SDK commands such as Streamline's evaluate need for one image. On Vulkan, view is destroyed after the commands. */
	struct ExternalImageInfo
	{
		void* native = nullptr;
		std::uint64_t view = 0;
		std::uint64_t state = 0;
		std::uint32_t format = 0;
		std::uint32_t width = 0, height = 0, mips = 1, layers = 1;
		std::uint32_t usage = 0;  // Vulkan: VkImageUsageFlags
	};

	/**
	 * @brief The native command list and per-image info for vendor SDK commands in a pass's Record. Images are in the common state.
	 * The pass must retain a_views until the GPU is past them.
	 */
	bool DescribeForCommands(rhi::CommandList& a_list, std::span<const rhi::Resource> a_images, void*& a_nativeList, std::vector<ExternalImageInfo>& a_infos,
		std::shared_ptr<const void>& a_views, std::string& a_why);

	/** @brief Runs a_callback now on the graphics queue under the queue lock. Matches dxvkEnqueueQueueCallback. */
	HRESULT __stdcall RunOnGraphicsQueue(ID3D11Device* a_device, void (*a_callback)(void*, VkQueue_T*), void* a_user);
	/** @} */

	/** @brief How WrapTexture treats the image. */
	enum class WrapFlags : std::uint32_t
	{
		None = 0,
		/**
		 * @brief Fill the record and make the handle only: no initialize, no zero, no submission and no wait. CPU-only, so any thread
		 * (the revision lane). The image is still Undefined, so its graph resource must say so and give its first contents.
		 */
		DeferInitialize = 1,
	};

	/**
	 * @brief A native texture handle over a port's BasicRHI image, so D3D11 users and graph imports reach the same image.
	 * Without DeferInitialize it moves the image into Common and zeroes it (or uploads a_data), and waits for that.
	 */
	winrt::com_ptr<ID3D11Resource> WrapTexture(const rhi::Resource& a_image, const DxvkOrgInteropTextureWrap& a_wrap, DxvkOrgInteropResourceInfo& a_info,
		const char* a_name, const D3D11_SUBRESOURCE_DATA* a_data, WrapFlags a_flags = WrapFlags::None);

	/** @brief A native buffer handle over a port's BasicRHI buffer. Null on failure. */
	winrt::com_ptr<ID3D11Buffer> WrapBuffer(const rhi::Resource& a_buffer, const D3D11_BUFFER_DESC& a_desc, const char* a_name);

	/**
	 * @brief Records and submits a_record now on the graphics queue, between full memory barriers, through the one path for work
	 * outside the graphs (ORG's external client). a_record runs exactly once on success.
	 */
	bool EmitCommands(void (*a_record)(void*, rhi::CommandList&), void* a_user, const rhi::TimelinePoint* a_waits, std::uint32_t a_waitCount,
		const rhi::TimelinePoint* a_signals, std::uint32_t a_signalCount, const char* a_label);

	/**
	 * @name Texture streaming shells, as DXVK's dxvkStream* exports. A shell is a storage-less texture whose window the streamer sets.
	 * Engine draws do not read shells in native mode, so the streamer decides every window.
	 */
	/** @{ */
	bool CreateStreamShell(const D3D11_TEXTURE2D_DESC& a_desc, ID3D11Texture2D** a_texture, ID3D11ShaderResourceView** a_view);
	HRESULT __stdcall StreamEnableView(ID3D11ShaderResourceView* a_shell, void(__stdcall* a_missing)(std::uint64_t), void(__stdcall* a_gone)(std::uint64_t),
		const D3D11_SUBRESOURCE_DATA*(__stdcall* a_fill)(std::uint64_t, BOOL), std::uint64_t a_user);
	void __stdcall StreamSetWindow(ID3D11ShaderResourceView* a_shell, ID3D11ShaderResourceView* a_window);
	ID3D11ShaderResourceView* __stdcall StreamGetWindow(ID3D11ShaderResourceView* a_shell);
	std::uint64_t __stdcall StreamGetBinds(ID3D11ShaderResourceView* a_shell);
	/** @} */

	/** @brief Waits for the last present and destroys the swap chain and back buffer. Runs at window close. */
	void Shutdown();
}

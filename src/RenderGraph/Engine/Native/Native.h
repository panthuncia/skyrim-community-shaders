#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <span>
#include <string_view>

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
}

namespace org
{
	class ExternalTextureResource;
	class GloballyIndexedResource;
}

/**
 * @brief Native mode (org-full-port.md Phase 6, CS_ORG_NATIVE=1): no DXVK. CS creates a BasicRHI Vulkan device itself, the engine's
 * device, immediate context and swap chain are tripwire handles over it, the render graph runs natively (its epochs submit to the
 * device's queues, no DXVK stream), and ORG presents. A development option: the DXVK build stays the default.
 *
 * Startup, in order (each stage logs one "[ORG native] stage" line, so a failed bring-up shows how far it got):
 * 1. switches (SKSEPlugin_Load, after CommunityShaders.env): the port switches native mode needs are turned on. One set to 0, or
 *    a switch that conflicts (CS_NATIVE_D3D11, CS_ORG_OWNED_FRAME, CS_ORG=0), is a startup error that names it.
 * 2. checks (Hooks::InstallEarlyHooks): DXVK is not loaded; a feature that still needs DXVK is a startup error that names it.
 * 3. device, 4. swap chain, 5. handles (the game's D3D11CreateDeviceAndSwapChain, SE 75589 / AE 77396): BasicRHI's device, ORG's
 *    swap chain on the game's window with the back buffer, and the device, context and swap chain handles the engine stores
 *    (RendererData +0x38/+0x40, window 0's swap chain, and the device and context globals SE 524729/524771, AE 411348/411391).
 * 6. engine init (the InitD3D hook): the engine's fields are checked to hold the handles.
 * 7. render graph (State::Setup, RenderGraphRuntime::Initialize): the graph host runs natively on BasicRHI's device.
 * 8. first present: the back buffer is copied into the swap chain image and presented.
 *
 * What still needs DXVK fails at its first use with a message that names it, never by falling back to DXVK.
 */
namespace RenderGraph::Engine::Native
{
	/** @brief Whether CS_ORG_NATIVE=1 asks for native mode. Any thread. */
	bool Requested();

	/** @brief Whether native mode made its device (stage 3 passed). Any thread. */
	bool Active();

	/** @brief Logs a startup stage: "[ORG native] stage <a_stage>: <a_detail>". */
	void Stage(std::string_view a_stage, std::string_view a_detail = {});

	/**
	 * @brief A startup error: logs it and ends the process with a message box that names what still needs DXVK (a_dependency) and
	 * why. Native mode never falls back to DXVK.
	 */
	[[noreturn]] void Fail(std::string_view a_dependency, std::string_view a_why);

	/**
	 * @brief Stage 1, right after CommunityShaders.env is applied and before any other switch is read: turns on every port switch
	 * native mode needs (in the process environment), and fails on a switch that conflicts. Does nothing without CS_ORG_NATIVE=1.
	 */
	void ApplySwitches();

	/**
	 * @brief On D3D12 (CS_ORG_NATIVE_BACKEND=d3d12), the Drawcall Limit Fix switches ApplySwitches set for what has no D3D12 port yet, as
	 * "NAME=value (why)" items for DCLF's log line; those the environment set already are named as kept. Empty otherwise.
	 */
	std::string D3D12DclfOff();

	/** @brief Stage 2, in Hooks::InstallEarlyHooks after the features' boot state is known: fails on what still needs DXVK. */
	void CheckStartup();

	/** @brief The directory of CS's runtime DLLs (dxcompiler.dll, dxil.dll): the plugin's CommunityShaders/bin. */
	std::filesystem::path RuntimeDirectory();

	/**
	 * @brief Stage 3, in place of D3D11CreateDeviceAndSwapChain: makes BasicRHI's device and ORG's swap chain, and returns tripwire
	 * handles for the device, the immediate context and the swap chain. Any failure is a startup error (Fail).
	 */
	HRESULT CreateDeviceAndSwapChain(IDXGIAdapter* a_adapter, DXGI_SWAP_CHAIN_DESC* a_desc, IDXGISwapChain** a_swapChain, ID3D11Device** a_device,
		D3D_FEATURE_LEVEL* a_featureLevel, ID3D11DeviceContext** a_context);

	/** @brief Stage 4, in the InitD3D hook after the engine's own: checks that the engine's fields hold the handles, and logs them. */
	void AfterInitD3D();

	/** @brief BasicRHI's device once stage 3 made it, else null. RenderGraphRuntime::Initialize runs the graph on it. */
	std::shared_ptr<rhi::DevicePtr> Device();

	/**
	 * @brief RenderGraphRuntime::Initialize, once the render graph is up on the native device and before any import: the back buffer
	 * becomes a graph-owned slot with one version per present slot, and the present
	 * copies it through ORG's external client (org-native-frame-graph.md, "Phase 4 item 5 lane").
	 */
	void OnGraphReady();

	/**
	 * @brief RenderGraphRuntime::Initialize, once the runtime shader compiler exists (after OnGraphReady): loads GenerateMips' compute
	 * programs. Called before the compiler was made, they loaded nothing and engine GenerateMips calls made no mips.
	 */
	void OnShaderCompilerReady();

	/**
	 * @brief What DXVK's dxvkGetInteropResourceInfo answers for its objects, for the textures native mode made (the back buffer, and
	 * textures the native device made): the Vulkan image and the layout it rests in between graph epochs. False for anything else.
	 */
	bool DescribeResource(const IUnknown* a_object, DxvkOrgInteropResourceInfo& a_info);

	/**
	 * @name Graph-owned textures (org-native-frame-graph.md, Phase 2). The native device
	 * sorts each texture it makes:
	 * - exempt: only a shader resource, made with its contents, not DYNAMIC. Its contents go in through a transfer on the graphics
	 *   queue that ends in the shader-read state, where it rests; graphs read it untracked (GraphOwnership::
	 *   ExternalImmutableShaderResource). The context's first write promotes it to tracked, through the build manager.
	 * - tracked: everything else. Once the render graph is up it is a persistent ORG texture (org::PixelBuffer, never aliased),
	 *   resting in Common between executions while external home states are on. Textures made before the graph, and ones ORG
	 *   cannot make as D3D11 asked, stay host-made BasicRHI images that the graph imports (logged once per reason).
	 */
	/** @{ */
	/** @brief Whether native mode is active and sorts its textures as above. Any thread. */
	bool OwnedResources();

	/**
	 * @brief The one lookup the imports use in native mode: the graph-owned ORG texture behind a_object (a texture or view handle,
	 * a port's handle around one, or a streamed texture's shell, which answers its window). Null for anything else: an exempt or
	 * host-made texture, or one native mode did not make; the caller imports it as before then. Any thread.
	 */
	std::shared_ptr<org::GloballyIndexedResource> GraphTextureOf(const IUnknown* a_object);

	/** @brief GraphTextureOf for the image a description named (DxvkOrgInteropImageInfo::image), for Drawcall Limit Fix's imports. */
	std::shared_ptr<org::GloballyIndexedResource> GraphTextureOfImage(std::uint64_t a_image);

	/**
	 * @brief An exempt texture as a graph resource (the FrameAnchors::ImmutableTexture seam): one per texture, made on the first call
	 * once its transfer is done (waited for here), with its shader resource views in a_srvFormat (UNKNOWN: the texture's own). Passes
	 * may only read it. Null when the texture is not exempt; the caller imports it as before then. Any thread.
	 */
	std::shared_ptr<org::ExternalTextureResource> ImmutableTextureOf(const IUnknown* a_texture, DXGI_FORMAT a_srvFormat);

	/**
	 * @brief The graph resource of an exempt texture the context wrote after creation (promoted to tracked), which passes that read
	 * it must declare: ImmutableTextureOf's, made here when nothing asked for it before. a_object may be a view. Null for any other
	 * texture. Any thread.
	 */
	std::shared_ptr<org::ExternalTextureResource> PromotedTextureOf(const IUnknown* a_object);

	/**
	 * @brief Whether a_object (a texture or view handle, or a streamed texture's shell) is a texture native mode made that graphs must
	 * track when they read it: graph-owned (GraphTextureOf), or host-made (made before the graph was up, kept host-made, or a wrapped
	 * image). False for an exempt texture, promoted or not (PromotedTextureOf answers those), and for anything native mode did not
	 * make. Any thread.
	 */
	bool IsTrackedTexture(const IUnknown* a_object);
	/** @} */

	/** @brief What native mode knows of a resource it made, from its D3D11 description (DescribeForCensus). */
	struct ResourceFacts
	{
		std::uint64_t serial = 0;     // unique for the session
		std::uint32_t bindFlags = 0;  // D3D11_BIND_*
		std::uint32_t usage = 0;      // D3D11_USAGE
		std::uint32_t dimension = 0;  // D3D11_RESOURCE_DIMENSION
		bool initialData = false;     // made with initial data
		bool written = false;         // the immediate context wrote it since (only marked while a census or the validator is on)
	};

	/**
	 * @brief The facts of the native resource behind a_object (a resource or view handle, or a port's handle around one). False when
	 * native mode did not make it. For the undeclared-access validator's exemptions. Any thread.
	 */
	bool DescribeForCensus(const void* a_object, ResourceFacts& a_out);

	/**
	 * @brief Stage 6 and every frame after: the engine's Present on the swap chain handle. Copies the back buffer into the acquired
	 * swap chain image on the graphics queue, after the frame's graph work, and presents it.
	 */
	HRESULT Present(UINT a_syncInterval, UINT a_flags);

	/**
	 * @brief HDR Display's colour space change (UpdateSwapChainColorSpace) in native mode: HDR10 (ST.2084 PQ, Rec. 2020) or SDR on
	 * BasicRHI's swap chain, through Engine::ConfigureSwapchain, under the present lock once the queued presents are done. The swap
	 * chain handle answers no IDXGISwapChain3/4, so HDR Display calls this instead of SetColorSpace1. Logs the colour space it ends in.
	 * @return Whether the swap chain now presents in the colour space asked for (false before stage 4, or when the display path
	 * refuses HDR10).
	 */
	bool SetSwapChainColorSpace(bool a_hdr);

	/**
	 * @name The profilers' view of the native Vulkan device (GPU counters, RenderGraphRuntime::Initialize), what DXVK's
	 * dxvkGetInteropDeviceInfo and dxvkEnqueueQueueCallback give them in guest mode.
	 */
	/** @{ */
	/**
	 * @brief The native Vulkan device as DXVK describes its own: instance, physical device, device, graphics queue and family, and
	 * the loader's vkGetInstanceProcAddr. No extension or feature list (BasicRHI keeps none). False on D3D12 or before stage 3.
	 */
	bool DescribeVulkanDevice(DxvkOrgInteropDeviceInfo& a_info);

	/** @brief The LUID of the adapter the native device was made on (LowPart and HighPart as one 64-bit pattern), or 0. */
	std::uint64_t AdapterLuid();

	/** @brief An image external work (Streamline's evaluates) reads or writes: a D3D11 handle the native device made, and how. */
	struct ExternalImage
	{
		enum class Use : std::uint8_t
		{
			Read,   // sampled
			Write,  // unordered access
		};
		ID3D11Resource* resource = nullptr;
		Use use = Use::Read;
	};

	/**
	 * @brief What external work's commands get for each ExternalImage, in their order. native: the VkImage or the ID3D12Resource*.
	 * view: on Vulkan a VkImageView of every mip and layer (a depth image's depth aspect), made for this work and destroyed after it;
	 * 0 on D3D12. state: the layout the image is in for the commands (VkImageLayout, or the D3D12 legacy resource state Streamline's
	 * tags take). format: VkFormat or DXGI_FORMAT.
	 */
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
	 * @brief Runs external commands (a_record, given the native command list: a VkCommandBuffer, or the ID3D12GraphicsCommandList
	 * Streamline's proxy device made) on a list of their own, on the context's route: in queue order after the graph's work so far,
	 * each image moved into its state by its ledger (or from Common, an image no graph tracks). a_keep is held until the GPU is
	 * past the work. False, logged once per label, when an image is not the native device's or nothing was submitted. Render
	 * thread.
	 */
	bool RunExternal(const std::string& a_label, std::span<const ExternalImage> a_images,
		const std::function<void(void* a_nativeList, std::span<const ExternalImageInfo> a_images)>& a_record, std::shared_ptr<const void> a_keep = nullptr);

	/**
	 * @brief Runs a_callback now with the native graphics queue, holding the lock every queue access takes, so it lands between the
	 * submissions made before and after the call. The signature of DXVK's dxvkEnqueueQueueCallback (a_device is ignored). E_FAIL on
	 * D3D12 or before stage 3.
	 */
	HRESULT __stdcall RunOnGraphicsQueue(ID3D11Device* a_device, void (*a_callback)(void*, VkQueue_T*), void* a_user);
	/** @} */

	/**
	 * @brief RenderGraphRuntime::WrapTexture in native mode: a native texture handle over an image a port made with BasicRHI, so its
	 * D3D11 readers (the engine's fields, the context's copies and clears) and the graph's import reach the same image. The image is
	 * moved into GENERAL, its contents uploaded (a_data) or zeroed, as DXVK's wrap does. a_info answers as dxvkCreateTextureFromVkImage
	 * would. Null on failure (logged).
	 */
	winrt::com_ptr<ID3D11Resource> WrapTexture(const rhi::Resource& a_image, const DxvkOrgInteropTextureWrap& a_wrap, DxvkOrgInteropResourceInfo& a_info,
		const char* a_name, const D3D11_SUBRESOURCE_DATA* a_data);

	/** @brief RenderGraphRuntime::WrapBuffer in native mode: a native buffer handle over a port's BasicRHI buffer. Null on failure. */
	winrt::com_ptr<ID3D11Buffer> WrapBuffer(const rhi::Resource& a_buffer, const D3D11_BUFFER_DESC& a_desc, const char* a_name);

	/**
	 * @brief RenderGraphRuntime::EmitCommands in native mode, on Vulkan and D3D12: a_record (BasicRHI commands, nothing bound at the
	 * start) into a command list of its own on the graphics queue, submitted now with a_waits and a_signals (the device's
	 * timelines), between full memory barriers. a_record runs exactly once when this returns true, never when it returns false.
	 */
	bool EmitCommands(void (*a_record)(void*, rhi::CommandList&), void* a_user, const rhi::TimelinePoint* a_waits, std::uint32_t a_waitCount,
		const rhi::TimelinePoint* a_signals, std::uint32_t a_signalCount, const char* a_label);

	/**
	 * @name Texture streaming's shells (TextureStreaming/Streamer.cpp), what DXVK's dxvkStream* exports do. A shell is a texture
	 * handle without storage and its SRV. The streamer sets a window (an SRV of a texture with the resident mips) on it, and
	 * DescribeResource answers the window's image for the shell. Engine draws do not read shells in native mode, so no bind
	 * counts and no missing-window callback: the streamer's scheduler (DCLF) decides every window. The gone callback runs when
	 * the shell's SRV is destroyed, as DXVK runs it in the view's destructor.
	 */
	/** @{ */
	bool CreateStreamShell(const D3D11_TEXTURE2D_DESC& a_desc, ID3D11Texture2D** a_texture, ID3D11ShaderResourceView** a_view);
	HRESULT __stdcall StreamEnableView(ID3D11ShaderResourceView* a_shell, void(__stdcall* a_missing)(std::uint64_t), void(__stdcall* a_gone)(std::uint64_t),
		const D3D11_SUBRESOURCE_DATA*(__stdcall* a_fill)(std::uint64_t, BOOL), std::uint64_t a_user);
	void __stdcall StreamSetWindow(ID3D11ShaderResourceView* a_shell, ID3D11ShaderResourceView* a_window);
	ID3D11ShaderResourceView* __stdcall StreamGetWindow(ID3D11ShaderResourceView* a_shell);
	std::uint64_t __stdcall StreamGetBinds(ID3D11ShaderResourceView* a_shell);
	/** @} */

	/** @brief Teardown, at the game window's close: waits for the last present and destroys the swap chain and the back buffer. */
	void Shutdown();
}

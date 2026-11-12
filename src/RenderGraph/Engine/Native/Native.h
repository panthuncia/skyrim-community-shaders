#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string_view>

#include <winrt/base.h>

struct DxvkOrgInteropResourceInfo;
struct VkCommandBuffer_T;
struct VkSemaphoreSubmitInfo;
struct DxvkOrgInteropTextureWrap;

namespace rhi
{
	class DevicePtr;
	class Resource;
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
	 * @brief What DXVK's dxvkGetInteropResourceInfo answers for its objects, for the textures native mode made (the back buffer, and
	 * textures the native device made): the Vulkan image and the layout it rests in between graph epochs. False for anything else.
	 */
	bool DescribeResource(const IUnknown* a_object, DxvkOrgInteropResourceInfo& a_info);

	/**
	 * @brief Stage 6 and every frame after: the engine's Present on the swap chain handle. Copies the back buffer into the acquired
	 * swap chain image on the graphics queue, after the frame's graph work, and presents it.
	 */
	HRESULT Present(UINT a_syncInterval, UINT a_flags);

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
	 * @brief RenderGraphRuntime::EmitExternalCommands in native mode, which DXVK's stream did: a_record into a command buffer of
	 * its own on the graphics queue, submitted now with a_waits and a_signals, between full memory barriers. False on failure.
	 */
	bool EmitExternalCommands(void (*a_record)(void*, VkCommandBuffer_T*), void* a_user, const VkSemaphoreSubmitInfo* a_waits, std::uint32_t a_waitCount,
		const VkSemaphoreSubmitInfo* a_signals, std::uint32_t a_signalCount);

	/** @brief Teardown, at the game window's close: waits for the last present and destroys the swap chain and the back buffer. */
	void Shutdown();
}

#pragma once

#include <d3d11.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <winrt/base.h>

struct DxvkOrgInteropResourceInfo;

namespace org::services
{
	class ShaderCompiler;
}

namespace org
{
	class PersistentGraphHost;
	class RenderGraph;
	class Resource;
}

/**
 * @brief OpenRenderGraph running on DXVK's own Vulkan device.
 *
 * BasicRHI adopts the VkDevice and graphics queue DXVK created for the game's D3D11
 * device, so graph passes record native Vulkan work that shares memory and the
 * queue with the translated D3D11 stream. Synchronization is coarse: each epoch
 * flushes DXVK (everything recorded so far is submitted ahead of the graph), then
 * the graph submits on the same queue under DXVK's submission lock, with full
 * memory barriers at its entry and exit. Later D3D11 work is therefore ordered
 * after the graph without semaphores.
 *
 * Graph-owned outputs are handed to D3D11 as wrappers over the graph's own Vulkan
 * objects (WrapBuffer). Game resources the graph reads directly (Drawcall Limit Fix's
 * static geometry and textures) are described through DescribeResource, which marks
 * them stable in DXVK; nothing else the game or DXVK owns is pinned or relocated.
 *
 * Header deliberately free of ORG/BasicRHI/Vulkan includes: those require volk
 * ahead of any Vulkan header, which only the implementation files arrange.
 */
class RenderGraphRuntime
{
public:
	/**
	 * @brief Where in the frame an epoch runs. The graph holds every feature's passes and each epoch
	 * executes all of them (an empty pass is effectively free), so passes record work only in their own
	 * segment (CurrentSegment) and include it in their invocation revision.
	 */
	enum class Segment : std::uint32_t
	{
		LightCulling,  // Light Limit Fix, before the main pass
		ZPrepass,      // Drawcall Limit Fix's depth, at the first draw of the main (deferred) pass
		MainOpaque,    // Drawcall Limit Fix's colour, before the deferred composite
		DebugView,     // Drawcall Limit Fix debug view, before the deferred composite
	};

	static RenderGraphRuntime& Get();

	/** @brief Asks DXVK to enable the device features BasicRHI needs. Call before the D3D11 device exists. */
	static void RequestDeviceFeatures(HMODULE a_dxvkD3D11);

	/** @brief Adopts DXVK's Vulkan device. Safe to call repeatedly; returns whether the runtime is active. */
	bool Initialize();

	/** @brief Retires all graph work and releases the adopted device (never destroys it). */
	void Shutdown();

	/** @brief Whether graph epochs can run. */
	bool IsActive() const;

	/** @brief Why the runtime is inactive (empty when active). */
	const std::string& GetDisabledReason() const;

	/** @brief The persistent graph host; null when inactive. */
	org::PersistentGraphHost* Host();

	/**
	 * @brief Runs one graph epoch at the current point of the D3D11 stream.
	 * @param a_beforePrepare Called with the graph's services active, before its passes
	 *        prepare: queue this frame's uploads (BUFFER_UPLOAD) here.
	 * @return false when inactive or the graph failed; callers fall back to their D3D11 path.
	 */
	bool ExecuteEpoch(Segment a_segment, const std::function<void(org::RenderGraph&)>& a_beforePrepare = {});

	/** @brief The segment of the epoch being executed (valid while passes prepare and record). */
	Segment CurrentSegment() const { return segment; }

	/**
	 * @brief The Vulkan resource behind a D3D11 buffer, texture or SRV (dxvkGetInteropResourceInfo).
	 * Marks it stable in DXVK; keep a reference on the D3D11 object while using the result. Fails for
	 * buffers the game can map. Synchronizes with DXVK's worker thread: resolve once and cache.
	 */
	bool DescribeResource(IUnknown* a_object, DxvkOrgInteropResourceInfo& a_info);

	/**
	 * @brief Runtime DXC compilation to SPIR-V with BasicRHI's ABI (ORGModuleServices), content-addressed
	 * with a disk cache under Data/ShaderCache/ORG. Null when inactive, or when the build or the
	 * installation has no DXC (dxcompiler.dll beside the DXVK DLLs).
	 */
	org::services::ShaderCompiler* ShaderCompiler();

	/** @brief Wraps a materialized graph-owned buffer as a D3D11 buffer (DEFAULT usage, no CPU access). */
	winrt::com_ptr<ID3D11Buffer> WrapBuffer(org::Resource& a_buffer, const D3D11_BUFFER_DESC& a_desc);

	~RenderGraphRuntime();

private:
	RenderGraphRuntime() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
	std::string disabledReason = "not initialized";
	Segment segment = Segment::LightCulling;
	bool attempted = false;
};

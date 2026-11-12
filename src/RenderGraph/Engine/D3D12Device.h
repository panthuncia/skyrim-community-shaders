#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace rhi
{
	class DevicePtr;
	class Swapchain;
}

namespace RenderGraph::Engine
{
	/** @brief What the native mode asks of its D3D12 device. */
	struct D3D12DeviceOptions
	{
		// The D3D12 debug layer (it also turns DRED on). For development runs: it is slow.
		bool debug = false;
		// DRED breadcrumbs and page-fault reports, logged on device removal (DXVK_DEBUG=hang, CS_ORG_CRASH_MARKERS).
		bool crashMarkers = false;
		std::uint32_t framesInFlight = 3;
		// The adapter to create the device on (its LUID, LowPart and HighPart as one 64-bit pattern). 0 lets BasicRHI choose.
		std::uint64_t adapterLuid = 0;
	};

	/**
	 * @brief CS_ORG_NATIVE=1 with CS_ORG_NATIVE_BACKEND=d3d12: creates the render graph's D3D12 device through BasicRHI.
	 *
	 * The device has a graphics, a compute and a copy queue, and shader-visible heaps the size of the Vulkan descriptor buffer's:
	 * 1,000,000 CBV/SRV/UAV descriptors and 2048 samplers. CS shaders bind through SM 6.6 ResourceDescriptorHeap and
	 * SamplerDescriptorHeap, and push data is root constants.
	 *
	 * The Agility SDK: only an executable can export D3D12SDKVersion, so CS loads one through a device factory
	 * (ID3D12SDKConfiguration1::CreateDeviceFactory) when CS_ORG_D3D12_SDK names its version. Its D3D12Core.dll is read from
	 * CS_ORG_D3D12_SDK_PATH, else from Data/SKSE/Plugins/CommunityShaders/D3D12. Without it the system's D3D12 runtime is used.
	 *
	 * The device must have Shader Model 6.6, resource binding tier 3 and enhanced barriers. Without one of them it is released,
	 * and a_reason names what is missing.
	 *
	 * Call it where the native mode creates its device, then Backend::SetActive(rhi::Backend::D3D12).
	 * @return false with a_reason set when the device could not be made.
	 */
	bool CreateD3D12Device(const D3D12DeviceOptions& a_options, std::shared_ptr<rhi::DevicePtr>& a_device, std::string& a_reason);

	/** @brief The swap chain settings the native mode presents with. */
	struct D3D12PresentOptions
	{
		// HDR10 output: the swap chain takes the ST.2084 PQ colour space with Rec. 2020 primaries (SetColorSpace1).
		bool hdr = false;
		// Frames the CPU may queue ahead of the GPU. The frame paces on the waitable object (WaitForFrameLatency).
		std::uint32_t maximumFrameLatency = 2;
	};

	/**
	 * @brief Applies HDR and frame latency to a swap chain of a D3D12 device. Also correct on Vulkan, through the same BasicRHI
	 * calls. HDR that the display path cannot take is logged and left off.
	 * @return false when a setting was refused (logged).
	 */
	bool ConfigureSwapchain(rhi::Swapchain& a_swapchain, const D3D12PresentOptions& a_options);
}

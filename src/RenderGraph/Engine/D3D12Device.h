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
	struct D3D12DeviceOptions
	{
		// Also turns DRED on. Slow, for development runs.
		bool debug = false;
		// DRED breadcrumbs and page-fault reports, logged on device removal.
		bool crashMarkers = false;
		std::uint32_t framesInFlight = 3;
		// The adapter's LUID as one 64-bit pattern. 0 lets BasicRHI choose.
		std::uint64_t adapterLuid = 0;
		// Streamline initialized by CS for this device, or null.
		const void* streamlineHost = nullptr;
	};

	/**
	* @brief Creates the render graph's D3D12 device through BasicRHI, for CS_ORG_NATIVE_BACKEND=d3d12.
	*
	* Only an executable can export D3D12SDKVersion, so CS loads the Agility SDK through a device factory when CS_ORG_D3D12_SDK
	* names its version. Without it the system's D3D12 runtime is used.
	* The device needs Shader Model 6.6, resource binding tier 3 and enhanced barriers.
	* @return False with a_reason set when the device could not be made.
	*/
	bool CreateD3D12Device(const D3D12DeviceOptions& a_options, std::shared_ptr<rhi::DevicePtr>& a_device, std::string& a_reason);

	struct D3D12PresentOptions
	{
		// HDR10 output in the ST.2084 PQ colour space with Rec. 2020 primaries.
		bool hdr = false;
		// The frame paces on the waitable object.
		std::uint32_t maximumFrameLatency = 2;
	};

	/**
	* @brief Also correct on Vulkan, through the same BasicRHI calls. HDR the display cannot take is logged and left off.
	* @return False when a setting was refused.
	*/
	bool ConfigureSwapchain(rhi::Swapchain& a_swapchain, const D3D12PresentOptions& a_options);
}

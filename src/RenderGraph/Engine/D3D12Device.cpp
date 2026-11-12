#include "RenderGraph/Engine/D3D12Device.h"

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/Backend.h"
#	include "RenderGraph/Switches.h"

#	include <rhi.h>

#	if defined(CS_ORG_HAS_D3D12)
#		include <d3d12.h>
#		include <winrt/base.h>
#	endif

#	include <filesystem>

namespace RenderGraph::Engine
{
#	if defined(CS_ORG_HAS_D3D12)
	namespace
	{
		// The Vulkan descriptor buffer's sizes, so a shader indexes the same ranges on both backends.
		constexpr std::uint32_t kResourceDescriptors = 1'000'000;
		constexpr std::uint32_t kSamplerDescriptors = 2048;

		// From d3d12.h, spelled out so no GUID library is needed.
		constexpr GUID kClsidSdkConfiguration = { 0x7cda6aca, 0xa03e, 0x49c8, { 0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce } };
		constexpr GUID kIidDeviceFactory = { 0x61f307d3, 0xd34e, 0x4e7c, { 0x83, 0x74, 0x3b, 0xa4, 0xde, 0x23, 0xcc, 0xcb } };

		/**
		 * @brief The Agility SDK's device factory when CS_ORG_D3D12_SDK names a version, else null (the system runtime).
		 * a_reason is set when the SDK was asked for and could not be loaded. That is an error, not a fallback.
		 */
		winrt::com_ptr<::IUnknown> AgilityFactory(std::string& a_reason)
		{
			if (!Switches::IsSet("CS_ORG_D3D12_SDK"))
				return nullptr;
			const auto version = static_cast<UINT>(Switches::Number("CS_ORG_D3D12_SDK", 0));
			if (!version) {
				a_reason = fmt::format("CS_ORG_D3D12_SDK={} is not an Agility SDK version number (its D3D12SDKVersion, such as 618)",
					Switches::Value("CS_ORG_D3D12_SDK"));
				return nullptr;
			}
			std::filesystem::path directory = Switches::IsSet("CS_ORG_D3D12_SDK_PATH") ? std::filesystem::path(Switches::Value("CS_ORG_D3D12_SDK_PATH")) :
			                                                                             std::filesystem::path("Data/SKSE/Plugins/CommunityShaders/D3D12");
			std::error_code error;
			directory = std::filesystem::absolute(directory, error);
			if (error || !std::filesystem::exists(directory / "D3D12Core.dll", error)) {
				a_reason = fmt::format("CS_ORG_D3D12_SDK={} asks for an Agility SDK, but {} has no D3D12Core.dll", version, directory.string());
				return nullptr;
			}
			using GetInterface = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
			const HMODULE d3d12 = ::LoadLibraryW(L"d3d12.dll");
			const auto getInterface = d3d12 ? reinterpret_cast<GetInterface>(::GetProcAddress(d3d12, "D3D12GetInterface")) : nullptr;
			if (!getInterface) {
				a_reason = "the system's d3d12.dll has no D3D12GetInterface, so it cannot load an Agility SDK";
				return nullptr;
			}
			winrt::com_ptr<ID3D12SDKConfiguration1> configuration;
			HRESULT hr = getInterface(kClsidSdkConfiguration, __uuidof(ID3D12SDKConfiguration1), configuration.put_void());
			if (FAILED(hr) || !configuration) {
				a_reason = fmt::format("the system's D3D12 runtime has no ID3D12SDKConfiguration1 (hr {:#010x}), so it is too old for an Agility SDK in a plugin",
					static_cast<std::uint32_t>(hr));
				return nullptr;
			}
			// The path ends in a separator, as D3D12SDKPath does.
			const std::string path = directory.string() + "\\";
			winrt::com_ptr<::IUnknown> factory;
			hr = configuration->CreateDeviceFactory(version, path.c_str(), kIidDeviceFactory, factory.put_void());
			if (FAILED(hr) || !factory) {
				a_reason = fmt::format("the Agility SDK {} in {} could not be loaded (hr {:#010x}). Its D3D12Core.dll must be that version.", version,
					directory.string(), static_cast<std::uint32_t>(hr));
				return nullptr;
			}
			logger::info("[ORG] D3D12: Agility SDK {} from {}", version, directory.string());
			return factory;
		}

		const char* ShaderModelName(::ShaderModel a_model)
		{
			switch (a_model) {
			case ::ShaderModel::SM_6_0:
				return "6.0";
			case ::ShaderModel::SM_6_1:
				return "6.1";
			case ::ShaderModel::SM_6_2:
				return "6.2";
			case ::ShaderModel::SM_6_3:
				return "6.3";
			case ::ShaderModel::SM_6_4:
				return "6.4";
			case ::ShaderModel::SM_6_5:
				return "6.5";
			case ::ShaderModel::SM_6_6:
				return "6.6";
			case ::ShaderModel::SM_6_7:
				return "6.7";
			case ::ShaderModel::SM_6_8:
				return "6.8";
			case ::ShaderModel::SM_6_9:
				return "6.9";
			case ::ShaderModel::SM_6_10:
				return "6.10";
			default:
				return "unknown";
			}
		}

		/** @brief What the device lacks that CS needs; empty when it has everything. */
		std::string MissingFeatures(rhi::Device& a_device)
		{
			::AdapterFeatureInfo adapter{};
			::ShaderFeatureInfo shader{};
			::EnhancedBarriersFeatureInfo barriers{};
			adapter.header.pNext = &shader.header;
			shader.header.pNext = &barriers.header;
			if (a_device.QueryFeatureInfo(&adapter.header) != rhi::Result::Ok)
				return "BasicRHI could not read the device's features";
			logger::info("[ORG] D3D12 device: {} (vendor {:#06x}, device {:#06x}), {} MiB video memory, Shader Model {}, binding tier 3 {}, "
						 "enhanced barriers {}, shader-visible heaps of {} resources and {} samplers",
				adapter.name, adapter.vendorId, adapter.deviceId, adapter.dedicatedVideoMemory >> 20, ShaderModelName(shader.maxShaderModel),
				shader.unboundedDescriptorTables ? "yes" : "no", barriers.enhancedBarriers ? "yes" : "no", shader.maxShaderVisibleResourceDescriptors,
				shader.maxShaderVisibleSamplerDescriptors);
			// CS's shaders index ResourceDescriptorHeap and SamplerDescriptorHeap directly.
			if (shader.maxShaderModel < ::ShaderModel::SM_6_6)
				return fmt::format("the device has Shader Model {}, and CS's shaders need 6.6 (ResourceDescriptorHeap)", ShaderModelName(shader.maxShaderModel));
			if (!shader.unboundedDescriptorTables)
				return "the device is below resource binding tier 3, which Shader Model 6.6's descriptor heap indexing needs";
			// BasicRHI's D3D12 backend records every barrier as an enhanced barrier.
			if (!barriers.enhancedBarriers)
				return "the device has no enhanced barriers (update the GPU driver, or set CS_ORG_D3D12_SDK to load an Agility SDK)";
			if (shader.maxShaderVisibleResourceDescriptors && shader.maxShaderVisibleResourceDescriptors < kResourceDescriptors)
				return fmt::format("the device's shader-visible heap holds {} descriptors, and the render graph needs {}",
					shader.maxShaderVisibleResourceDescriptors, kResourceDescriptors);
			return {};
		}
	}
#	endif

	bool CreateD3D12Device(const D3D12DeviceOptions& a_options, std::shared_ptr<rhi::DevicePtr>& a_device, std::string& a_reason)
	{
		a_device.reset();
#	if !defined(CS_ORG_HAS_D3D12)
		(void)a_options;
		a_reason = "this build has no D3D12 backend (built with CS_ORG_D3D12=OFF)";
		return false;
#	else
		auto fail = [&](std::string a_why) {
			a_device.reset();
			a_reason = "D3D12 device: " + std::move(a_why);
			logger::error("[ORG] {}", a_reason);
			return false;
		};

		std::string agilityError;
		const auto factory = AgilityFactory(agilityError);
		if (!agilityError.empty())
			return fail(std::move(agilityError));

		rhi::DeviceCreateInfo ci{};
		ci.backend = rhi::Backend::D3D12;
		ci.framesInFlight = a_options.framesInFlight;
		ci.descriptorHeapCapacities.preferredResources = kResourceDescriptors;
		ci.descriptorHeapCapacities.minResources = kResourceDescriptors;
		ci.descriptorHeapCapacities.preferredSamplers = kSamplerDescriptors;
		ci.descriptorHeapCapacities.minSamplers = kSamplerDescriptors;
		ci.enableDebug = a_options.debug;
		ci.enableCrashMarkers = a_options.crashMarkers;
		ci.d3d12DeviceFactory = factory.get();
		if (a_options.adapterLuid) {
			ci.adapterLuid = a_options.adapterLuid;
			ci.requireAdapterLuid = true;
		}

		// Streamline's interposer stays off: CS loads Streamline itself (Features/Upscaling/Streamline).
		a_device = std::make_shared<rhi::DevicePtr>();
		if (rhi::CreateD3D12Device(ci, *a_device, false) != rhi::Result::Ok || !*a_device)
			return fail(factory ? "rhi::CreateD3D12Device failed with the Agility SDK device factory (see BasicRHI's log lines above)" :
			                      "rhi::CreateD3D12Device failed on the system's D3D12 runtime (see BasicRHI's log lines above; CS_ORG_D3D12_SDK loads an Agility SDK)");
		if (auto missing = MissingFeatures(a_device->Get()); !missing.empty())
			return fail(std::move(missing));
		logger::info("[ORG] D3D12 device created: debug layer {}, DRED {}, {} frames in flight, {} runtime", a_options.debug ? "on" : "off",
			a_options.debug || a_options.crashMarkers ? "on" : "off", a_options.framesInFlight, factory ? "Agility SDK" : "system");
		a_reason.clear();
		return true;
#	endif
	}

	bool ConfigureSwapchain(rhi::Swapchain& a_swapchain, const D3D12PresentOptions& a_options)
	{
		bool ok = true;
		const auto wanted = a_options.hdr ? rhi::SwapchainColorSpace::Hdr10St2084 : rhi::SwapchainColorSpace::SrgbNonlinear;
		if (a_options.hdr && !a_swapchain.IsColorSpaceSupported(wanted)) {
			logger::warn("[ORG] HDR is on, but the display path does not take HDR10 (ST.2084, Rec. 2020). Presenting SDR.");
			ok = false;
		} else if (a_swapchain.SetColorSpace(wanted) != rhi::Result::Ok) {
			logger::error("[ORG] The swap chain refused the {} colour space", a_options.hdr ? "HDR10" : "SDR");
			ok = false;
		}
		if (a_options.maximumFrameLatency && a_swapchain.SetMaximumFrameLatency(a_options.maximumFrameLatency) != rhi::Result::Ok) {
			logger::error("[ORG] The swap chain refused a frame latency of {}", a_options.maximumFrameLatency);
			ok = false;
		}
		logger::info("[ORG] {} swap chain: {}, frame latency {}{}", Backend::Name(Backend::Active()), a_options.hdr && ok ? "HDR10" : "SDR",
			a_options.maximumFrameLatency, a_swapchain.FrameLatencyWaitableObject() ? " (waitable object)" : "");
		return ok;
	}
}

#else  // !CS_HAS_RENDER_GRAPH

namespace RenderGraph::Engine
{
	bool CreateD3D12Device(const D3D12DeviceOptions&, std::shared_ptr<rhi::DevicePtr>&, std::string& a_reason)
	{
		a_reason = "this build has no render graph";
		return false;
	}

	bool ConfigureSwapchain(rhi::Swapchain&, const D3D12PresentOptions&) { return false; }
}

#endif

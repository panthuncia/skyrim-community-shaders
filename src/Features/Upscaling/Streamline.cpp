#include "Streamline.h"
#include "Features/DrawcallLimitFix.h"
#include "Features/Upscaling.h"
#include "Snapshot/CoreFrame.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/FrameCamera.h"
#include "Snapshot/Publication.h"

#include "DXVKInterop.h"
#include "DxvkPresenter.h"

#include "../../DxvkLoader.h"
#include "Utils/SwitchFile.h"
#include "../../Globals.h"
#include "../../RenderGraph/Backend.h"
#include "../../RenderGraph/Engine/Native/Native.h"
#include "../../RenderGraph/Engine/Native/RenderThread.h"
#include "../../RenderGraph/Backend.h"
#include "../../RenderGraph/RenderGraphRuntime.h"
#include "ORGUpscaling.h"
#include "../../RenderGraph/Switches.h"
#include "../../State.h"
#include "../../Utils/Game.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <DirectXPackedVector.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_set>

#define NV_WINDOWS
#pragma warning(push)
#pragma warning(disable: 4471 5103)
#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_device_wrappers.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_fsr.h>
#include <sl_fsr_g.h>
#include <sl_xess.h>
#include <sl_matrix_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_version.h>
#pragma warning(pop)

namespace
{
	struct SLState
	{
		HMODULE interposer = nullptr;

		PFun_slInit* slInit = nullptr;
		PFun_slIsFeatureSupported* slIsFeatureSupported = nullptr;
		PFun_slGetNewFrameToken* slGetNewFrameToken = nullptr;
		PFun_slSetTagForFrame* slSetTagForFrame = nullptr;
		PFun_slSetConstants* slSetConstants = nullptr;
		PFun_slEvaluateFeature* slEvaluateFeature = nullptr;
		PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
		PFun_slSetFeatureLoaded* slSetFeatureLoaded = nullptr;

		PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings = nullptr;
		PFun_slDLSSSetOptions* slDLSSSetOptions = nullptr;
		PFun_slReflexSetOptions* slReflexSetOptions = nullptr;
		PFun_slReflexSleep* slReflexSleep = nullptr;
		PFun_slPCLSetMarker* slPCLSetMarker = nullptr;
		PFun_slDLSSGSetOptions* slDLSSGSetOptions = nullptr;
		PFun_slDLSSGGetState* slDLSSGGetState = nullptr;
		PFun_slFSRSetOptions* slFSRSetOptions = nullptr;
		PFun_slFSRFrameGenerationSetOptions* slFSRFrameGenerationSetOptions = nullptr;
		PFun_slFSRGetFrameGenState* slFSRGetFrameGenState = nullptr;
		PFun_slFSRFrameGenerationDiscardPreparedFrame* slFSRFrameGenerationDiscardPreparedFrame = nullptr;
		PFun_slFSRFrameGenerationOwnsSwapchain* slFSRFrameGenerationOwnsSwapchain = nullptr;
		PFun_slFSRFrameGenerationCompleteSwapchainTeardown* slFSRFrameGenerationCompleteSwapchainTeardown = nullptr;
		PFun_slXeSSSetOptions* slXeSSSetOptions = nullptr;

		sl::ViewportHandle viewport{ 0 };

		// The render frame's Streamline frame id: its render identity (Snapshot::Render().frame). Tags, constants and the
		// render-thread PCL markers use it. See Streamline::BeginRenderFrame.
		std::atomic<uint32_t> renderFrameId = { 0 };
		// The simulation frame ids of the newest game updates, by update (snapshot id). The game thread writes them at its input poll
		// (SimFrameId); the render frame that first renders an update's snapshot reads its id (BeginRenderFrame).
		static constexpr std::size_t kSimFrames = 16;
		std::array<std::atomic<std::uint64_t>, kSimFrames> simFrameUpdate{};
		std::array<std::atomic<uint32_t>, kSimFrames> simFrameId{};
		// Native's render thread: the frame id its Reflex sleep used, taken by the render frame that follows (0: none).
		std::atomic<uint32_t> sleptRenderFrameId = { 0 };
		// Reflex sleeps each frame: low-latency mode or a frame limit is set (UpdateReflex).
		std::atomic<bool> reflexSleeps = { false };
		// PCL markers only feed Reflex and DLSS-G latency. With neither active, emitting them is pure
		// per-frame cross-DLL overhead -- 7 slGetNewFrameToken+slPCLSetMarker pairs per frame on a path
		// the D3D11 branch does not have at all. Streamline is now always initialised for in-game
		// switching, so this has to be gated on use rather than on initialisation.
		std::atomic<bool> latencyMarkersNeeded = { false };
		// Frame generation also needs them for sl.common to recycle its input copies, and sl.common goes on making those once a
		// plugin has asked for the tags, after the plugin is unloaded too. So they stay on from the first load (2.4 GB of copies otherwise).
		std::atomic<bool> frameGenerationHasLoaded = { false };

		// Disable dispatch after an SEH fault to prevent repeated crashes.

		bool reflexCacheValid = false;
		sl::ReflexMode reflexCachedMode = sl::ReflexMode::eOff;
		uint32_t reflexCachedFrameLimitUs = 0;

		bool dlssgModeCached = false;
		bool dlssgModeOn = false;
		uint32_t dlssgCachedDisplayW = 0, dlssgCachedDisplayH = 0;
		uint32_t dlssgCachedNumFrames = 0;
		bool dlssgCachedAuto = false;
		bool dlssgCachedDynamic = false;
		float dlssgCachedDynamicFps = 0.0f;
		std::atomic<uint32_t> dlssgMaxFramesToGenerate = 0;
		std::atomic<bool> dlssgDynamicSupported = false;
		// SL-VSYNC-011 in the Streamline sample: DLSS-G reports whether vsync is usable while it is
		// generating, and the sample disables its VSync checkbox and forces the setting off when it
		// is not. Latched from DLSSGState every query.
		std::atomic<bool> dlssgVsyncSupported{ false };
		std::atomic<uint32_t> frameGenerationMultiplier = 1;
		// Running total of frames the FSR-FG swapchain has presented (real + generated). Differenced
		// over time this is the true post-FG frame rate; the per-present multiplier above reads 1 on
		// any present that had no prepared frame, so sampling it reports an un-doubled value.
		std::atomic<uint64_t> fsrTotalPresentedFrames = 0;

		// Present requires either a valid or passthrough tag every frame.
		bool dlssgTaggedThisFrame = false;
		std::atomic<bool> dlssgCloneTagsPrimed{ false };

		// Whether the render pass prepared an interpolation frame for FSR-FG this present.
		// Written from the render thread in EvaluateFSRFrameGen and read at present, so it is
		// atomic rather than a plain bool like the DLSS-G flag above.
		std::atomic<bool> fsrfgPreparedThisFrame{ false };

	} g_sl;


	// Feature load changes are applied only while the swapchain is torn down.
	std::atomic<bool> g_dlssgDesiredLoaded{ false };
	std::atomic<bool> g_dlssgCurrentlyLoaded{ false };
	std::atomic<bool> g_fsrfgDesiredLoaded{ false };
	std::atomic<bool> g_fsrfgCurrentlyLoaded{ false };
	std::atomic<bool> g_fsrfgOwnsPresent{ false };

	// Keep this free of C++ unwinding because it executes inside __try.
	bool ReconcileFgFeatureLoad(sl::Feature a_feature, std::atomic<bool>& a_desired, std::atomic<bool>& a_current)
	{
		const bool want = a_desired.load(std::memory_order_acquire);
		if (want == a_current.load(std::memory_order_acquire))
			return true;
		if (!g_sl.slSetFeatureLoaded)
			return false;
		if (g_sl.slSetFeatureLoaded(a_feature, want) != sl::Result::eOk)
			return false;
		a_current.store(want, std::memory_order_release);
		if (a_feature == sl::kFeatureFSR_G && !want)
			g_fsrfgOwnsPresent.store(false, std::memory_order_release);
		if (want) {
			// Safe: render thread is blocked on the present lock during swapchain teardown/recreation.
			if (a_feature == sl::kFeatureDLSS_G) {
				g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void*&>(g_sl.slDLSSGSetOptions));
				g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", reinterpret_cast<void*&>(g_sl.slDLSSGGetState));
				// The options and state calls are made unchecked from here on.
				if (!g_sl.slDLSSGSetOptions || !g_sl.slDLSSGGetState)
					return false;
			} else if (a_feature == sl::kFeatureFSR_G) {
				g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationSetOptions", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationSetOptions));
				g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRGetFrameGenState", reinterpret_cast<void*&>(g_sl.slFSRGetFrameGenState));
				g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationDiscardPreparedFrame", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationDiscardPreparedFrame));
				g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationOwnsSwapchain", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationOwnsSwapchain));
				g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationCompleteSwapchainTeardown", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationCompleteSwapchainTeardown));
			}
		}
		return true;
	}

	// Runs between the swap chain's destruction and creation.
	bool SwapchainTornDown()
	{
		// Per-swapchain options are invalid after teardown.
		g_sl.dlssgModeCached = false;
		g_sl.dlssgModeOn = false;

		g_sl.dlssgCloneTagsPrimed.store(false, std::memory_order_release);

		if (g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire)) {
			if (g_sl.slFSRFrameGenerationCompleteSwapchainTeardown) {
				bool teardownComplete = false;
				const bool releaseFeatureContext = !g_fsrfgDesiredLoaded.load(std::memory_order_acquire);
				teardownComplete = g_sl.slFSRFrameGenerationCompleteSwapchainTeardown(releaseFeatureContext);
				if (!teardownComplete)
					return false;
			} else {
				g_fsrfgCurrentlyLoaded.store(false, std::memory_order_release);
			}
		}

		const bool dlssgReconciled = ReconcileFgFeatureLoad(
			sl::kFeatureDLSS_G, g_dlssgDesiredLoaded, g_dlssgCurrentlyLoaded);
		const bool fsrfgReconciled = ReconcileFgFeatureLoad(
			sl::kFeatureFSR_G, g_fsrfgDesiredLoaded, g_fsrfgCurrentlyLoaded);
		return dlssgReconciled && fsrfgReconciled;
	}

	// Suppress only diagnostics that are both expected in this integration and repeated often
	// enough to drown the log. Anything reporting a feature degrading or failing outright stays
	// visible: those are exactly the lines that explain a silently non-working feature.
	//
	// Deliberately NOT suppressed, having each cost real debugging time:
	//   "is NOT supported, plugin will not function properly" — a hook Streamline could not
	//       install. Three of these fire on the DLSS-G path (CmdBindPipeline,
	//       CmdBindDescriptorSets, BeginCommandBuffer).
	//   "Invalid backbuffer resource extent"                  — a resource tagged with a 0x0
	//       extent, which sl.dlss_g sanitises and carries on from.
	bool IsBenignSLWarning(const char* a_msg)
	{
		if (!a_msg)
			return false;
		static constexpr const char* kBenign[] = {
			"setAsyncFrameMarker is not implemented",
			"RSync will not run because it was not initialized",
			"some DX/VK APIs were invoked before slInit",
			"reseting frame timer",
		};
		for (const char* needle : kBenign) {
			if (std::strstr(a_msg, needle))
				return true;
		}
		return false;
	}

	// Streamline repeats most warnings every frame. Emit each distinct message once so a real
	// diagnostic is visible without the log turning into a per-frame stream.
	bool ShouldLogSLWarningOnce(const char* a_msg)
	{
		static std::mutex s_mutex;
		static std::unordered_set<size_t> s_seen;
		const size_t key = std::hash<std::string_view>{}(a_msg);
		std::lock_guard<std::mutex> lock(s_mutex);
		return s_seen.insert(key).second;
	}

	void LogCallback(sl::LogType a_type, const char* a_msg)
	{
		static const bool s_verbose = Switches::On("CS_SL_VERBOSE");
		if (s_verbose) {
			logger::info("[Streamline/SL] {}", a_msg);
			return;
		}
		if (a_type == sl::LogType::eWarn && IsBenignSLWarning(a_msg))
			return;
		switch (a_type) {
		case sl::LogType::eError:
			logger::warn("[Streamline/SL] {}", a_msg);
			break;
		case sl::LogType::eWarn:
			// Previously logged at debug, which the default sink drops — so every surviving
			// Streamline warning was invisible anyway, and un-suppressing one changed nothing.
			if (ShouldLogSLWarningOnce(a_msg))
				logger::warn("[Streamline/SL] {}", a_msg);
			break;
		default:
			logger::trace("[Streamline/SL] {}", a_msg);
			break;
		}
	}

	std::filesystem::path GetStreamlineDir()
	{
		return DxvkLoader::GetRuntimeDir();
	}

	template <typename T>
	bool Resolve(T*& a_fn, const char* a_name)
	{
		a_fn = reinterpret_cast<T*>(GetProcAddress(g_sl.interposer, a_name));
		if (!a_fn)
			logger::warn("[Streamline] missing interposer export '{}'", a_name);
		return a_fn != nullptr;
	}
}

Streamline* Streamline::GetSingleton()
{
	static Streamline singleton;
	return &singleton;
}

// RenderDoc's Vulkan layer does not expose VK_NVX_binary_import or VK_NVX_image_view_handle, and the
// interposer's vkCreateDevice fails with VK_ERROR_EXTENSION_NOT_PRESENT without them, which takes DXVK's
// device (normal and safe mode alike) and the game down with it. DLSS cannot run under RenderDoc anyway,
// so Streamline stands down and DXVK talks to the real loader.
static bool RenderDocLoaded()
{
	return GetModuleHandleW(L"renderdoc.dll") != nullptr;
}

void Streamline::PreloadInterposer()
{
	// Preload before DXVK creates VkInstance so its Vulkan loader aliases the interposer.
	if (g_sl.interposer)
		return;
	if (RenderDocLoaded()) {
		logger::info("[Streamline] renderdoc.dll is loaded: interposer not preloaded, DXVK uses the real Vulkan loader");
		return;
	}
	const auto slDir = GetStreamlineDir();
	if (slDir.empty())
		return;
	const auto interposerPath = (slDir / L"sl.interposer.dll").wstring();
	g_sl.interposer = LoadLibraryExW(interposerPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	logger::info("[Streamline] interposer preload for DXVK Vulkan interposition: {}",
		g_sl.interposer ? "mapped" : "FAILED (DXVK uses real driver)");
	if (!g_sl.interposer)
		return;
	// slInit must precede DXVK's VkInstance creation.
	Initialize();
}

// XeSS's Vulkan library builds its descriptor set layouts with VK_DESCRIPTOR_TYPE_MUTABLE_EXT, and Streamline reports it supported
// on devices without mutable descriptors (Pascal), where those layouts are invalid. False when the device lacks them.
static bool VulkanMutableDescriptorsSupported(PFN_vkGetInstanceProcAddr a_gipa, VkInstance a_instance, VkPhysicalDevice a_physicalDevice)
{
	if (!a_gipa || !a_instance || !a_physicalDevice)
		return false;
	const auto enumExts = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(a_gipa(a_instance, "vkEnumerateDeviceExtensionProperties"));
	const auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(a_gipa(a_instance, "vkGetPhysicalDeviceFeatures2"));
	if (!enumExts || !getFeatures2)
		return false;
	uint32_t count = 0;
	if (enumExts(a_physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS)
		return false;
	std::vector<VkExtensionProperties> extensions(count);
	if (enumExts(a_physicalDevice, nullptr, &count, extensions.data()) != VK_SUCCESS)
		return false;
	// A structure of an extension the device lacks must not be chained into the query.
	const bool listed = std::any_of(extensions.begin(), extensions.begin() + count, [](const VkExtensionProperties& a_ext) {
		return std::strcmp(a_ext.extensionName, VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME) == 0;
	});
	if (!listed)
		return false;
	VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT };
	VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	features.pNext = &mutableFeatures;
	getFeatures2(a_physicalDevice, &features);
	return mutableFeatures.mutableDescriptorType == VK_TRUE;
}

// Probe with the system loader before slInit decides which FG plugin to load. A nonzero a_luid (native mode) checks only the
// adapter the device is made on, so a second GPU's optical flow does not load DLSS-G for a device that lacks it.
static bool ProbeDLSSGHardware(uint64_t a_luid)
{
	if (Switches::On("CS_FORCE_FSR_FG")) {
		logger::info("[Streamline] CS_FORCE_FSR_FG=1: hardware probe reports no DLSS-G (FSR-FG path forced)");
		return false;
	}

	wchar_t sysDir[MAX_PATH]{};
	if (!GetSystemDirectoryW(sysDir, MAX_PATH))
		return false;
	const auto vkPath = std::wstring(sysDir) + L"\\vulkan-1.dll";
	HMODULE vk = LoadLibraryExW(vkPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!vk) {
		logger::warn("[Streamline] system vulkan-1.dll unavailable - assuming no DLSS-G hardware");
		return false;
	}

	bool found = false;
	auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vk, "vkGetInstanceProcAddr"));
	auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(GetProcAddress(vk, "vkCreateInstance"));
	if (gipa && createInstance) {
		VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
		app.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		ici.pApplicationInfo = &app;
		VkInstance instance = VK_NULL_HANDLE;
		if (createInstance(&ici, nullptr, &instance) == VK_SUCCESS && instance) {
			auto enumDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
			auto enumExts = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
			auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
			auto getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
			if (enumDevices && enumExts && (!a_luid || getProps2)) {
				uint32_t count = 0;
				enumDevices(instance, &count, nullptr);
				std::vector<VkPhysicalDevice> devices(count);
				enumDevices(instance, &count, devices.data());
				for (auto dev : devices) {
					if (a_luid) {
						VkPhysicalDeviceIDProperties ids{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
						VkPhysicalDeviceProperties2 props{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids };
						getProps2(dev, &props);
						uint64_t luid = 0;
						std::memcpy(&luid, ids.deviceLUID, sizeof(luid));
						if (!ids.deviceLUIDValid || luid != a_luid)
							continue;
					}
					uint32_t extCount = 0;
					enumExts(dev, nullptr, &extCount, nullptr);
					std::vector<VkExtensionProperties> exts(extCount);
					enumExts(dev, nullptr, &extCount, exts.data());
					for (const auto& e : exts) {
						if (std::strcmp(e.extensionName, "VK_NV_optical_flow") == 0) {
							found = true;
							break;
						}
					}
					if (found)
						break;
				}
			}
			if (destroyInstance)
				destroyInstance(instance, nullptr);
		}
	}
	FreeLibrary(vk);
	logger::info("[Streamline] hardware probe: DLSS-G-class GPU (VK_NV_optical_flow) {}{}", found ? "present" : "absent",
		a_luid ? " on the native device's adapter" : "");
	return found;
}

bool Streamline::Initialize()
{
	if (triedInit)
		return initialized;
	triedInit = true;

	// Native D3D12: Streamline on the native D3D12 device (asked before the device exists, so the requested backend). CS's sl.fsr and
	// sl.xess run there too; sl.fsr_g is Vulkan-only and stands down. DLSS and Reflex run.
	const bool d3d12 = native && RenderGraph::Backend::Requested() == rhi::Backend::D3D12;

	if (RenderDocLoaded()) {
		logger::info("[Streamline] renderdoc.dll is loaded: Streamline features disabled for this session");
		MarkUnavailable();
		return false;
	}

	const auto slDir = GetStreamlineDir();
	if (slDir.empty()) {
		logger::warn("[Streamline] could not resolve plugin directory");
		MarkUnavailable();
		return false;
	}

	const auto interposerPath = (slDir / L"sl.interposer.dll").wstring();
	if (!g_sl.interposer)
		g_sl.interposer = LoadLibraryExW(interposerPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!g_sl.interposer) {
		logger::info("[Streamline] sl.interposer.dll not present in '{}' — Streamline features disabled", slDir.string());
		MarkUnavailable();
		return false;
	}

	const bool resolved =
		Resolve(g_sl.slInit, "slInit") &&
		Resolve(g_sl.slIsFeatureSupported, "slIsFeatureSupported") &&
		Resolve(g_sl.slGetNewFrameToken, "slGetNewFrameToken") &&
		Resolve(g_sl.slSetTagForFrame, "slSetTagForFrame") &&
		Resolve(g_sl.slSetConstants, "slSetConstants") &&
		Resolve(g_sl.slEvaluateFeature, "slEvaluateFeature") &&
		Resolve(g_sl.slGetFeatureFunction, "slGetFeatureFunction");

	Resolve(g_sl.slSetFeatureLoaded, "slSetFeatureLoaded");
	if (!resolved) {
		FreeLibrary(g_sl.interposer);
		g_sl.interposer = nullptr;
		MarkUnavailable();
		return false;
	}

	const auto slDirWide = slDir.wstring();
	const wchar_t* pluginPaths[] = { slDirWide.c_str() };
	// The controller keeps at most one frame-generation feature loaded at runtime.
	dlssgHardware = ProbeDLSSGHardware(native ? nativeAdapterLuid : 0);

	// Frame generation needs Reflex and PCL. DLSS-G does not load without Reflex. FSR-FG needs PCL's present marker: without it
	// sl.common never recycles the input copies it makes each frame, and holds 32 frames of them (3.3 GB at 4K, past the budget of
	// a 16 GB card). They do make the interposer enable VK_NV_low_latency on DXVK's device, which Nsight Graphics refuses to capture,
	// so CS_STREAMLINE_REFLEX=0 leaves them out for a capture.
	const bool reflexRequested = Util::SwitchFile::Value("CS_STREAMLINE_REFLEX") != "0";
	// CS_STREAMLINE_DLSS=0 leaves DLSS and DLSS-G out. On a machine with an NVIDIA GPU, DLSS-G's plugin adds VK_NV_optical_flow to
	// the device even when DXVK runs on another vendor's GPU (DXVK_FILTER_DEVICE_NAME), and that device then fails to create.
	const bool dlssRequested = Util::SwitchFile::Value("CS_STREAMLINE_DLSS") != "0";
	// Native mode loads both frame generation plugins as guest does. Native unloads DLSS-G again before its first swap chain
	// (UnloadDLSSGBeforeFirstSwapchain), but slInit must load it so the device takes its extensions and queues.
	std::vector<sl::Feature> featuresToLoad = { sl::kFeatureFSR, sl::kFeatureXeSS, sl::kFeatureFSR_G };
	if (dlssRequested)
		featuresToLoad.push_back(sl::kFeatureDLSS);
	else
		logger::info("[Streamline] CS_STREAMLINE_DLSS=0: DLSS and DLSS-G are not loaded");
	if (reflexRequested) {
		featuresToLoad.push_back(sl::kFeatureReflex);
		featuresToLoad.push_back(sl::kFeaturePCL);
	}
	if (dlssgHardware && reflexRequested && dlssRequested)
		featuresToLoad.push_back(sl::kFeatureDLSS_G);
	else if (native)
		logger::info("[Streamline] DLSS-G is not loaded for the native device: {}", !dlssgHardware ? "its adapter has no optical flow hardware" :
																					 !dlssRequested ? "CS_STREAMLINE_DLSS=0" :
																									  "CS_STREAMLINE_REFLEX=0");
	if (reflexRequested)
		logger::info("[Streamline] Reflex, PCL and DLSS-G loaded (CS_STREAMLINE_REFLEX=0 leaves them out)");
	else
		logger::warn("[Streamline] CS_STREAMLINE_REFLEX=0: no Reflex, PCL or DLSS-G; FSR-FG would hold 32 frames of input copies in VRAM");

	sl::Preferences pref{};
	pref.renderAPI = d3d12 ? sl::RenderAPI::eD3D12 : sl::RenderAPI::eVulkan;
	pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	// Native mode: BasicRHI creates the device and calls Streamline's swap chain and command hooks itself (manual hooking).
	if (native)
		pref.flags |= sl::PreferenceFlags::eUseManualHooking;
	loadedFeatures.assign(featuresToLoad.begin(), featuresToLoad.end());
	pluginDirectory = slDirWide;
	pref.featuresToLoad = featuresToLoad.data();
	pref.numFeaturesToLoad = static_cast<uint32_t>(featuresToLoad.size());
	pref.pathsToPlugins = pluginPaths;
	pref.numPathsToPlugins = 1;
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0";
	pref.projectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
	if (Switches::On("CS_SL_VERBOSE"))
		pref.logLevel = sl::LogLevel::eVerbose;
	else
		pref.logLevel = sl::LogLevel::eDefault;
	pref.logMessageCallback = &LogCallback;

	const sl::Result res = g_sl.slInit(pref, sl::kSDKVersion);
	if (res != sl::Result::eOk) {
		logger::warn("[Streamline] slInit failed (result {}) — Streamline features disabled", static_cast<int>(res));
		FreeLibrary(g_sl.interposer);
		g_sl.interposer = nullptr;
		MarkUnavailable();
		return false;
	}

	initialized = true;
	logger::info("[Streamline] initialized on {}{} (SDK {}.{}.{})", d3d12 ? "D3D12" : "Vulkan", native ? " for the native device (manual hooking)" : "",
		SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
	return true;
}

bool Streamline::InitializeForNative(uint64_t a_adapterLuid)
{
	if (triedInit)
		return initialized && native;
	native = true;
	nativeAdapterLuid = a_adapterLuid;
	return Initialize();
}

void Streamline::UnloadDLSSGBeforeFirstSwapchain()
{
	if (!initialized || !native || std::ranges::find(loadedFeatures, static_cast<uint32_t>(sl::kFeatureDLSS_G)) == loadedFeatures.end())
		return;
	// Not supported on this adapter: Streamline refuses the change, and the plugin never attaches.
	const sl::Result result = g_sl.slSetFeatureLoaded ? g_sl.slSetFeatureLoaded(sl::kFeatureDLSS_G, false) : sl::Result::eErrorMissingOrInvalidAPI;
	g_dlssgDesiredLoaded.store(false, std::memory_order_release);
	g_dlssgCurrentlyLoaded.store(false, std::memory_order_release);
	logger::info("[Streamline] DLSS-G unloaded before the native device's first swap chain (result {}); it loads again at a presentation replacement",
		static_cast<int>(result));
}

void* Streamline::InterposerModule() const
{
	return initialized ? g_sl.interposer : nullptr;
}

void Streamline::SetNativeDevice(void* a_vkInstance, void* a_vkGetInstanceProcAddr, void* a_vkPhysicalDevice, uint64_t a_luid)
{
	if (!initialized || !native || vulkanDeviceSet)
		return;
	vulkanDeviceSet = true;
	sl::AdapterInfo adapter{};
	if (a_vkPhysicalDevice) {
		adapter.vkPhysicalDevice = a_vkPhysicalDevice;
	} else {
		adapter.deviceLUID = reinterpret_cast<uint8_t*>(&a_luid);
		adapter.deviceLUIDSizeInBytes = sizeof(a_luid);
	}
	const auto supported = [&](sl::Feature f) {
		if (std::ranges::find(loadedFeatures, static_cast<uint32_t>(f)) == loadedFeatures.end())
			return false;
		const sl::Result r = g_sl.slIsFeatureSupported(f, adapter);
		if (r != sl::Result::eOk)
			logger::info("[Streamline] feature {} unsupported on the native device (result {})", f, static_cast<int>(r));
		return r == sl::Result::eOk;
	};
	featureDLSS = supported(sl::kFeatureDLSS);
	featureReflex = supported(sl::kFeatureReflex);
	featureXeSS = supported(sl::kFeatureXeSS);
	if (featureXeSS && a_vkPhysicalDevice &&
		!VulkanMutableDescriptorsSupported(reinterpret_cast<PFN_vkGetInstanceProcAddr>(a_vkGetInstanceProcAddr), static_cast<VkInstance>(a_vkInstance),
			static_cast<VkPhysicalDevice>(a_vkPhysicalDevice))) {
		logger::info("[Streamline] XeSS off: the native Vulkan device has no mutable descriptors, which XeSS's Vulkan library needs");
		featureXeSS = false;
	}
	featureFSR = supported(sl::kFeatureFSR);
	featureDLSSG = dlssgHardware && supported(sl::kFeatureDLSS_G);
	featureFSRFG = supported(sl::kFeatureFSR_G);
	if (featureDLSS) {
		g_sl.slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", reinterpret_cast<void*&>(g_sl.slDLSSGetOptimalSettings));
		g_sl.slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", reinterpret_cast<void*&>(g_sl.slDLSSSetOptions));
		featureDLSS = g_sl.slDLSSSetOptions != nullptr;
	}
	if (featureReflex) {
		g_sl.slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", reinterpret_cast<void*&>(g_sl.slReflexSetOptions));
		g_sl.slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", reinterpret_cast<void*&>(g_sl.slReflexSleep));
		featureReflex = g_sl.slReflexSetOptions != nullptr && g_sl.slReflexSleep != nullptr;
	}
	g_sl.slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", reinterpret_cast<void*&>(g_sl.slPCLSetMarker));
	if (featureFSR) {
		g_sl.slGetFeatureFunction(sl::kFeatureFSR, "slFSRSetOptions", reinterpret_cast<void*&>(g_sl.slFSRSetOptions));
		featureFSR = g_sl.slFSRSetOptions != nullptr;
	}
	if (featureXeSS) {
		g_sl.slGetFeatureFunction(sl::kFeatureXeSS, "slXeSSSetOptions", reinterpret_cast<void*&>(g_sl.slXeSSSetOptions));
		featureXeSS = g_sl.slXeSSSetOptions != nullptr;
	}
	if (featureFSRFG) {
		g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationSetOptions", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationSetOptions));
		g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRGetFrameGenState", reinterpret_cast<void*&>(g_sl.slFSRGetFrameGenState));
		g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationDiscardPreparedFrame", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationDiscardPreparedFrame));
		g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationOwnsSwapchain", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationOwnsSwapchain));
		g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationCompleteSwapchainTeardown", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationCompleteSwapchainTeardown));
		featureFSRFG = g_sl.slFSRFrameGenerationSetOptions != nullptr && g_sl.slFSRGetFrameGenState != nullptr &&
		               g_sl.slFSRFrameGenerationCompleteSwapchainTeardown != nullptr;
	}
	// The functions resolve while the plugin is unloaded (UnloadDLSSGBeforeFirstSwapchain). It stays unloaded until the controller
	// loads it at a presentation replacement.
	if (featureDLSSG) {
		g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void*&>(g_sl.slDLSSGSetOptions));
		g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", reinterpret_cast<void*&>(g_sl.slDLSSGGetState));
		featureDLSSG = g_sl.slDLSSGSetOptions != nullptr && g_sl.slDLSSGGetState != nullptr;
	}
	if (!featureDLSSG)
		logger::info("[Streamline] DLSS-G unsupported on the native device: {}", !dlssgHardware ? "no optical flow hardware on its adapter" :
																				  std::ranges::find(loadedFeatures, static_cast<uint32_t>(sl::kFeatureDLSS_G)) == loadedFeatures.end() ?
																									   "not loaded (CS_STREAMLINE_DLSS or CS_STREAMLINE_REFLEX)" :
																									   "Streamline refused it on this adapter, or its functions are missing");
	// Loaded from slInit, as in guest. The controller unloads it at a presentation replacement while frame generation is off.
	if (featureFSRFG) {
		g_fsrfgDesiredLoaded.store(true, std::memory_order_release);
		g_fsrfgCurrentlyLoaded.store(true, std::memory_order_release);
	}
	// The vendor from the adapter, as SetVulkanDevice reads it from Vulkan: DXGI names the adapter the native device was made on.
	winrt::com_ptr<IDXGIFactory4> factory;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
		LUID luid{ static_cast<DWORD>(a_luid & 0xFFFFFFFFu), static_cast<LONG>(a_luid >> 32) };
		winrt::com_ptr<IDXGIAdapter1> dxgiAdapter;
		DXGI_ADAPTER_DESC1 desc{};
		if (a_luid && SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(dxgiAdapter.put()))) && SUCCEEDED(dxgiAdapter->GetDesc1(&desc))) {
			isNvidiaGPU = desc.VendorId == 0x10DE;
			isIntelGPU = desc.VendorId == 0x8086;
			constexpr uint32_t kFirstAdaDeviceID = 0x2600;
			isRTXBelow40Series = isNvidiaGPU && desc.DeviceId < kFirstAdaDeviceID;
		}
	}
	logger::info("[Streamline] native device feature support: DLSS={} Reflex={} FSR={} XeSS={} FSR-G={} DLSS-G={}; NVIDIA {}",
		featureDLSS, featureReflex, featureFSR, featureXeSS, featureFSRFG, featureDLSSG, isNvidiaGPU);
}

void Streamline::SetVulkanDevice()
{
	if (!initialized || vulkanDeviceSet)
		return;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk || !dxvk->IsAvailable()) {
		logger::warn("[Streamline] DXVK interop unavailable — cannot hand Vulkan device to SL");
		return;
	}

	vulkanDeviceSet = true;

	// Probe support against DXVK's physical device.
	sl::AdapterInfo adapter{};
	adapter.vkPhysicalDevice = dxvk->GetPhysicalDevice();
	const auto supported = [&](sl::Feature f) {
		const sl::Result r = g_sl.slIsFeatureSupported(f, adapter);
		if (r != sl::Result::eOk)
			logger::info("[Streamline] feature {} unsupported (result {})", f, static_cast<int>(r));
		return r == sl::Result::eOk;
	};

	featureDLSS = supported(sl::kFeatureDLSS);
	featureReflex = supported(sl::kFeatureReflex);
	featureDLSSG = supported(sl::kFeatureDLSS_G);
	featureXeSS = supported(sl::kFeatureXeSS);
	if (featureXeSS && !VulkanMutableDescriptorsSupported(dxvk->GetInstanceProcAddr(), dxvk->GetInstance(), dxvk->GetPhysicalDevice())) {
		logger::info("[Streamline] XeSS off: DXVK's Vulkan device has no mutable descriptors, which XeSS's Vulkan library needs");
		featureXeSS = false;
	}
	featureFSR = supported(sl::kFeatureFSR);
	featureFSRFG = supported(sl::kFeatureFSR_G);

	if (featureDLSS) {
		g_sl.slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", reinterpret_cast<void*&>(g_sl.slDLSSGetOptimalSettings));
		g_sl.slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", reinterpret_cast<void*&>(g_sl.slDLSSSetOptions));
		featureDLSS = g_sl.slDLSSSetOptions != nullptr;
	}
	if (featureReflex) {
		g_sl.slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", reinterpret_cast<void*&>(g_sl.slReflexSetOptions));
		g_sl.slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", reinterpret_cast<void*&>(g_sl.slReflexSleep));
		featureReflex = g_sl.slReflexSetOptions != nullptr && g_sl.slReflexSleep != nullptr;
	}
	g_sl.slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", reinterpret_cast<void*&>(g_sl.slPCLSetMarker));
	logger::info("[Streamline] PCL latency markers {}", g_sl.slPCLSetMarker ? "available" : "unavailable");
	if (featureDLSSG) {
		g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void*&>(g_sl.slDLSSGSetOptions));
		g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", reinterpret_cast<void*&>(g_sl.slDLSSGGetState));
		featureDLSSG = g_sl.slDLSSGSetOptions != nullptr && g_sl.slDLSSGGetState != nullptr;
	}
	if (featureFSR) {
		g_sl.slGetFeatureFunction(sl::kFeatureFSR, "slFSRSetOptions", reinterpret_cast<void*&>(g_sl.slFSRSetOptions));
		featureFSR = g_sl.slFSRSetOptions != nullptr;
	}
	if (featureFSRFG) {
		g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationSetOptions", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationSetOptions));
		g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRGetFrameGenState", reinterpret_cast<void*&>(g_sl.slFSRGetFrameGenState));
		g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationDiscardPreparedFrame", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationDiscardPreparedFrame));
		g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationOwnsSwapchain", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationOwnsSwapchain));
		g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationCompleteSwapchainTeardown", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationCompleteSwapchainTeardown));
		featureFSRFG = g_sl.slFSRFrameGenerationSetOptions != nullptr &&
			g_sl.slFSRFrameGenerationDiscardPreparedFrame != nullptr &&
			g_sl.slFSRFrameGenerationOwnsSwapchain != nullptr &&
			g_sl.slFSRFrameGenerationCompleteSwapchainTeardown != nullptr;
	}
	if (featureXeSS) {
		g_sl.slGetFeatureFunction(sl::kFeatureXeSS, "slXeSSSetOptions", reinterpret_cast<void*&>(g_sl.slXeSSSetOptions));
		featureXeSS = g_sl.slXeSSSetOptions != nullptr;
	}

	const bool frameGenerationInteropReady = FrameGen::DxvkPresenter::Get().HasFrameGenerationInterop();
	featureDLSSG = featureDLSSG && dlssgHardware && frameGenerationInteropReady;
	featureFSRFG = featureFSRFG && frameGenerationInteropReady &&
	               dxvk->FrameGenerationQueueInteropReady();

	if (featureDLSSG) {
		g_dlssgDesiredLoaded.store(true, std::memory_order_release);
		g_dlssgCurrentlyLoaded.store(true, std::memory_order_release);
	}
	if (featureFSRFG) {
		g_fsrfgDesiredLoaded.store(true, std::memory_order_release);
		g_fsrfgCurrentlyLoaded.store(true, std::memory_order_release);
	}

	logger::info("[Streamline] feature support: DLSS={} Reflex={} DLSS-G={} FSR={} FSR-G={} XeSS={} (FSR-FG fns {})",
		featureDLSS, featureReflex, featureDLSSG, featureFSR, featureFSRFG, featureXeSS,
		g_sl.slFSRFrameGenerationSetOptions && g_sl.slFSRFrameGenerationDiscardPreparedFrame &&
			g_sl.slFSRFrameGenerationOwnsSwapchain &&
			g_sl.slFSRFrameGenerationCompleteSwapchainTeardown ? "ok" : "missing");

	// Use Vulkan IDs because the D3D create hook may not see the adapter.
	if (auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
			dxvk->GetInstanceProcAddr()(dxvk->GetInstance(), "vkGetPhysicalDeviceProperties"))) {
		VkPhysicalDeviceProperties props{};
		getProps(dxvk->GetPhysicalDevice(), &props);
		isNvidiaGPU = props.vendorID == 0x10DE;
		// XeSS only runs its XMX path on Intel Arc. Everywhere else it falls back to DP4a, which
		// is a materially softer image at the same quality preset -- that is XeSS behaving as
		// designed, not the integration misbehaving, and it is worth saying so in the UI.
		isIntelGPU = props.vendorID == 0x8086;
		// Ada (RTX 40) and everything after it carry device IDs from 0x2600 up -- AD102 0x2684,
		// AD103 0x2704, AD107 0x2882, Blackwell GB202 0x2B85 -- while Ampere tops out in 0x25xx
		// and Turing sits in 0x1Exx-0x1Fxx. Test for Ada-or-newer directly. Allowlisting the
		// Ampere and Turing ranges instead meant every ID outside them fell through to "RTX 40+",
		// so Pascal, Maxwell and the tensor-less GTX 16xx were all reported as RTX 40+ and handed
		// preset M: a GTX 1080 Ti (0x1B06) logged as "RTX 40+ (M)".
		constexpr uint32_t kFirstAdaDeviceID = 0x2600;
		isRTXBelow40Series = isNvidiaGPU && props.deviceID < kFirstAdaDeviceID;
		logger::info("[Streamline] GPU vendor=0x{:04X} device=0x{:04X} -> DLSS preset group: {}",
			props.vendorID, props.deviceID,
			!isNvidiaGPU        ? "non-NVIDIA (default)" :
				isRTXBelow40Series ? "Ampere or older (J)" :
									 "Ada or newer (M)");
	}
}

// The last frame whose common constants were set on viewport 0. DLSS-G reads them, and Streamline wants them once a frame.
static std::atomic<uint32_t> g_viewport0ConstantsFrame{ UINT32_MAX };

static sl::FrameToken* TokenForFrame(uint32_t a_frameId)
{
	sl::FrameToken* token = nullptr;
	if (g_sl.slGetNewFrameToken(token, &a_frameId) != sl::Result::eOk)
		return nullptr;
	return token;
}

static sl::FrameToken* RenderFrameToken()
{
	return TokenForFrame(g_sl.renderFrameId.load(std::memory_order_acquire));
}

static sl::Result cs_SetTagForFrame(sl::FrameToken& a_token, const sl::ViewportHandle& a_viewport,
	const sl::ResourceTag* a_tags, uint32_t a_tagCount, VkCommandBuffer a_commandBuffer)
{
	sl::Result result = sl::Result::eErrorExceptionHandler;
	result = g_sl.slSetTagForFrame(a_token, a_viewport, a_tags, a_tagCount, a_commandBuffer);
	return result;
}

static sl::Result cs_EvaluateFeature(sl::Feature a_feature, sl::FrameToken& a_token,
	const sl::ViewportHandle& a_viewport, VkCommandBuffer a_commandBuffer)
{
	sl::Result result = sl::Result::eErrorExceptionHandler;
	const sl::BaseStructure* inputs[] = { &a_viewport };
	result = g_sl.slEvaluateFeature(
		a_feature, a_token, inputs, static_cast<uint32_t>(std::size(inputs)), a_commandBuffer);
	// Streamline replaces a successful evaluate's eOk with eWarnOutOfVRAM while the VRAM budget is exhausted
	// (sl.common's slEvaluateFeatureInternal): the work is recorded and the output written, so it is a success.
	if (result == sl::Result::eWarnOutOfVRAM) {
		static std::atomic<bool> s_logged{ false };
		if (!s_logged.exchange(true))
			logger::warn("[Streamline] evaluating over the VRAM budget (eWarnOutOfVRAM): outputs kept; expect stutter");
		result = sl::Result::eOk;
	}
	return result;
}

static sl::Result cs_DiscardFSRFrameGenerationPreparedFrame(const sl::ViewportHandle& a_viewport)
{
	sl::Result result = sl::Result::eErrorNotInitialized;
	if (!g_sl.slFSRFrameGenerationDiscardPreparedFrame)
		return result;

	result = g_sl.slFSRFrameGenerationDiscardPreparedFrame(a_viewport);
	return result;
}

// The frame index the next rendered frame will use. slReflexSleep, SimulationStart, the render-thread
// PCL markers, tags and constants must all agree on one index per frame, or Reflex never sees a frame
// close and its frame limiter falls back to open-loop sleeping.
//
// The simulation frame id of the game update in progress (the snapshot it will publish). Game thread. Each update takes one id,
// and ids only grow: at least the next render frame's id, and above the last update's. With the render thread, two updates can
// publish before one render (the first is never rendered); a shared id gave Reflex two SimulationStart markers for one frame.
// The render frame that first renders the update's snapshot takes this id when no render frame has taken it yet
// (BeginRenderFrame), so the update's simulation markers and its render's markers share one frame.
static uint32_t SimFrameId()
{
	static std::uint64_t s_update = UINT64_MAX;
	static uint32_t s_id = 0;
	const std::uint64_t update = Snapshot::LastGameId() + 1;
	if (update != s_update) {
		s_update = update;
		s_id = (std::max)(g_sl.renderFrameId.load(std::memory_order_acquire) + 1u, s_id + 1u);
		const std::size_t slot = update % g_sl.kSimFrames;
		g_sl.simFrameUpdate[slot].store(0, std::memory_order_release);
		g_sl.simFrameId[slot].store(s_id, std::memory_order_release);
		g_sl.simFrameUpdate[slot].store(update, std::memory_order_release);
	}
	return s_id;
}

// The simulation frame id a_update took, or 0 when it took none or a newer update reused its slot. Any thread.
static uint32_t SimFrameIdOf(std::uint64_t a_update)
{
	const std::size_t slot = a_update % g_sl.kSimFrames;
	if (g_sl.simFrameUpdate[slot].load(std::memory_order_acquire) != a_update)
		return 0;
	const uint32_t id = g_sl.simFrameId[slot].load(std::memory_order_acquire);
	return g_sl.simFrameUpdate[slot].load(std::memory_order_acquire) == a_update ? id : 0;
}

void Streamline::BeginRenderFrame()
{
	// Native mode records the prepare and presents on the submission thread, so a discard from this thread would race with them.
	// A present there that finds nothing prepared passes the frame through.
	if (!native && g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire))
		(void)DiscardFSRFrameGenerationPreparedFrame();
	// The render frame's identity, the id SimulationStart used for the snapshot it renders first. It must be strictly
	// increasing -- which the tag and constant keying and CS's own once-per-frame guards (s_evalFrameByVp / s_constFrameByVp)
	// require: a repeated index reads as "already evaluated" and silently dropped every second FSR-FG evaluation, measured
	// 11-12 evaluations per second against 20-21 rendered frames with output ~24% under target. A second call in one render
	// frame takes the next id. One id per frame also keeps sl.dlss_g from querying a frame Streamline has no record of at
	// present time:
	//   commonInterface.h:622[get] Unable to find 'common' constants for frame
	//   commonInterface.h:256[getTaggedResource] Failed to find global tag 'kBufferTypeDepth'
	// which interpolates without depth or motion vectors and shows up as an occasional generated
	// frame collapsing towards a corner.
	const uint32_t frame = static_cast<uint32_t>(Snapshot::Render().frame);
	const uint32_t previous = g_sl.renderFrameId.load(std::memory_order_acquire);
	uint32_t id = frame > previous ? frame : previous + 1u;
	// Native's render thread slept for this frame with the id it reserved (ReflexSleepRender).
	id = (std::max)(id, g_sl.sleptRenderFrameId.exchange(0, std::memory_order_acq_rel));
	// The first render of a game update's snapshot takes the update's simulation id when it is still ahead: Reflex then sees the
	// update's simulation and its render as one frame. A repeat has no simulation of its own.
	if (const auto& snapshot = Snapshot::Current(); snapshot && !Snapshot::Render().repeat)
		id = (std::max)(id, SimFrameIdOf(snapshot->id));
	g_sl.renderFrameId.store(id, std::memory_order_release);
	g_sl.dlssgTaggedThisFrame = false;
	g_sl.fsrfgPreparedThisFrame.store(false, std::memory_order_release);
}

bool Streamline::DiscardFSRFrameGenerationPreparedFrame()
{
	if (!g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire))
		return true;
	if (!g_sl.slFSRFrameGenerationDiscardPreparedFrame)
		return false;

	const sl::ViewportHandle fgViewport{ 1 };
	const sl::Result result = cs_DiscardFSRFrameGenerationPreparedFrame(fgViewport);
	if (result == sl::Result::eErrorInvalidState) {
		return true;
	}
	if (result != sl::Result::eOk) {
		logger::error("[Streamline] FSR prepared-frame discard failed (result {})",
			static_cast<int>(result));
		return false;
	}


	return true;
}

void Streamline::CaptureDLSSGPresentState()
{
	if (!initialized || !featureDLSSG ||
		!g_dlssgCurrentlyLoaded.load(std::memory_order_acquire) || !g_sl.dlssgModeOn)
		return;
	sl::DLSSGState state{};
	if (g_sl.slDLSSGGetState(g_sl.viewport, state, nullptr) == sl::Result::eOk) {
		// state.status says why DLSS-G is not generating. Report each distinct value once:
		// without it a non-functional DLSS-G is silent and simply halves the frame rate.
		static uint32_t s_dlssgLastPresented = UINT32_MAX;
		if (s_dlssgLastPresented != state.numFramesActuallyPresented) {
			const uint32_t prev = s_dlssgLastPresented;
			s_dlssgLastPresented = state.numFramesActuallyPresented;
			if (prev != UINT32_MAX && state.numFramesActuallyPresented < 2u)
				logger::warn("[Streamline] DLSS-G stopped generating: presenting {} (was {})",
					state.numFramesActuallyPresented, prev);
		}
		g_sl.dlssgVsyncSupported.store(state.bIsVsyncSupportAvailable == sl::Boolean::eTrue,
			std::memory_order_release);
		static uint32_t s_sampleTick = 0u;
		if ((++s_sampleTick % 600u) == 0u) {
			logger::info("[Streamline] DLSS-G presented {} frame(s) since last query (status 0x{:X}, max {}, vsyncSupportAvailable={})",
				state.numFramesActuallyPresented, static_cast<uint32_t>(state.status), state.numFramesToGenerateMax,
				state.bIsVsyncSupportAvailable == sl::Boolean::eTrue ? "yes" :
				state.bIsVsyncSupportAvailable == sl::Boolean::eFalse ? "no" : "invalid");
		}
		static uint32_t s_lastStatus = UINT32_MAX;
		const uint32_t status = static_cast<uint32_t>(state.status);
		if (status != s_lastStatus) {
			s_lastStatus = status;
			if (status == 0u) {
				logger::info("[Streamline] DLSS-G status OK (presenting {} frames, max {})",
					state.numFramesActuallyPresented, state.numFramesToGenerateMax);
			} else {
				logger::warn("[Streamline] DLSS-G NOT generating - status 0x{:X}{}{}{}{}{} (presenting {})",
					status,
					(status & 0x1) ? " resolutionTooLow" : "",
					(status & 0x2) ? " reflexNotDetectedAtRuntime" : "",
					(status & 0x4) ? " hdrFormatNotSupported" : "",
					(status & 0x8) ? " commonConstantsInvalid" : "",
					(status & 0x10) ? " getCurrentBackBufferIndexNotCalled" : "",
					state.numFramesActuallyPresented);
			}
		}
		g_sl.frameGenerationMultiplier.store(
			std::max(state.numFramesActuallyPresented, 1u), std::memory_order_release);
		g_sl.dlssgCloneTagsPrimed.store(true, std::memory_order_release);
	}
}

void Streamline::UpdateReflex(bool a_enable, bool a_boost, uint32_t a_frameLimitUs)
{
	if (globals::features::upscaling.settings.frameGeneration || g_dlssgCurrentlyLoaded.load(std::memory_order_acquire) ||
		g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire))
		g_sl.frameGenerationHasLoaded.store(true, std::memory_order_relaxed);
	g_sl.latencyMarkersNeeded.store(a_enable || g_sl.frameGenerationHasLoaded.load(std::memory_order_relaxed), std::memory_order_release);
	if (!initialized || !featureReflex)
		return;

	const sl::ReflexMode mode = !a_enable ? sl::ReflexMode::eOff :
	                            a_boost   ? sl::ReflexMode::eLowLatencyWithBoost :
	                                        sl::ReflexMode::eLowLatency;

	if (!g_sl.reflexCacheValid || g_sl.reflexCachedMode != mode || g_sl.reflexCachedFrameLimitUs != a_frameLimitUs) {
		sl::ReflexOptions options{};
		options.mode = mode;
		options.frameLimitUs = a_frameLimitUs;
		const sl::Result reflexRes = g_sl.slReflexSetOptions(options);
		if (reflexRes == sl::Result::eOk) {
			g_sl.reflexCachedMode = mode;
			g_sl.reflexCachedFrameLimitUs = a_frameLimitUs;
			g_sl.reflexCacheValid = true;
			logger::info("[Streamline] Reflex mode={} frameLimitUs={}",
				mode == sl::ReflexMode::eOff ? "off" :
				mode == sl::ReflexMode::eLowLatencyWithBoost ? "low-latency+boost" : "low-latency",
				a_frameLimitUs);
		} else {
			logger::warn("[Streamline] slReflexSetOptions failed (result {})", static_cast<int>(reflexRes));
		}
	}
	// slReflexSleep is where the frame limiter is actually enforced, so it has to run whenever a
	// limit is set -- not only when low-latency mode is on. Gating it on the mode is why the
	// FSR-FG path's cap "could not be applied": frameLimitUs was handed to Reflex and then never
	// given a chance to sleep on it, so the cap fell through to DXVK's limiter, which applies it
	// from the submission thread after the present instead of in the render loop.
	const bool sleeps = mode != sl::ReflexMode::eOff || a_frameLimitUs != 0u;
	g_sl.reflexSleeps.store(sleeps, std::memory_order_release);
	// With native's render thread the render thread sleeps (ReflexSleepRender): the frame limit paces renders, and the game thread
	// never waits for the render side's pacing.
	if (sleeps && !RenderGraph::Engine::Native::RenderThread::Running()) {
		// PollInputDevices can run more than once per rendered frame.
		static uint32_t s_lastSleepFrame = UINT32_MAX;
		const uint32_t simFrame = SimFrameId();
		if (s_lastSleepFrame != simFrame) {
			s_lastSleepFrame = simFrame;
			if (sl::FrameToken* token = TokenForFrame(simFrame)) {
				ZoneScopedN("CS.Streamline.ReflexSleep");
				const auto start = std::chrono::steady_clock::now();
				g_sl.slReflexSleep(*token);
				RenderGraph::Engine::Native::RenderThread::CountWait(RenderGraph::Engine::Native::RenderThread::Wait::ReflexSleep,
					std::chrono::steady_clock::now() - start);
			}
		}
	}
}

void Streamline::ReflexSleepRender()
{
	if (!initialized || !featureReflex || !g_sl.reflexSleeps.load(std::memory_order_acquire))
		return;
	// The render frame about to start: the next id, or the newest update's simulation id when that is ahead (the frame that renders
	// it takes it, BeginRenderFrame).
	const uint32_t id = (std::max)(g_sl.renderFrameId.load(std::memory_order_acquire) + 1u, SimFrameIdOf(Snapshot::LastGameId()));
	sl::FrameToken* token = TokenForFrame(id);
	if (!token)
		return;
	g_sl.sleptRenderFrameId.store(id, std::memory_order_release);
	ZoneScopedN("CS.Streamline.ReflexSleep");
	const auto start = std::chrono::steady_clock::now();
	g_sl.slReflexSleep(*token);
	RenderGraph::Engine::Native::RenderThread::CountWait(RenderGraph::Engine::Native::RenderThread::Wait::ReflexSleep,
		std::chrono::steady_clock::now() - start);
}

void Streamline::SetPCLMarker(PclMarker a_marker)
{
	if (!initialized || !g_sl.slPCLSetMarker)
		return;
	if (!g_sl.latencyMarkersNeeded.load(std::memory_order_acquire))
		return;

	// Emit SimulationStart once per game update (the snapshot it will publish), not once per input poll.
	uint32_t simFrame = 0;
	if (a_marker == PclMarker::SimulationStart) {
		static std::uint64_t s_lastUpdate = UINT64_MAX;
		const std::uint64_t update = Snapshot::LastGameId() + 1;
		if (s_lastUpdate == update)
			return;
		s_lastUpdate = update;
		simFrame = SimFrameId();
	}

	const bool renderThreadMarker =
		a_marker == PclMarker::RenderSubmitStart || a_marker == PclMarker::RenderSubmitEnd ||
		a_marker == PclMarker::PresentStart || a_marker == PclMarker::PresentEnd ||
		a_marker == PclMarker::TriggerFlash ||
		a_marker == PclMarker::SimulationEnd;
	sl::FrameToken* token = renderThreadMarker ?
	                            RenderFrameToken() :
	                            TokenForFrame(simFrame ? simFrame : SimFrameId());
	if (token)
		g_sl.slPCLSetMarker(static_cast<sl::PCLMarker>(a_marker), *token);
}

// Returns false until the engine camera matrices are finite and invertible.
static bool cs_IsFiniteMatrix(const Matrix& a_matrix)
{
	return std::isfinite(a_matrix._11) && std::isfinite(a_matrix._12) &&
	       std::isfinite(a_matrix._13) && std::isfinite(a_matrix._14) &&
	       std::isfinite(a_matrix._21) && std::isfinite(a_matrix._22) &&
	       std::isfinite(a_matrix._23) && std::isfinite(a_matrix._24) &&
	       std::isfinite(a_matrix._31) && std::isfinite(a_matrix._32) &&
	       std::isfinite(a_matrix._33) && std::isfinite(a_matrix._34) &&
	       std::isfinite(a_matrix._41) && std::isfinite(a_matrix._42) &&
	       std::isfinite(a_matrix._43) && std::isfinite(a_matrix._44);
}

static bool cs_BuildConstants(sl::Constants& a_consts, uint32_t a_outputWidth, uint32_t a_outputHeight,
	float a_jitterX, float a_jitterY)
{
	a_consts = {};
	a_consts.cameraAspectRatio = static_cast<float>(a_outputWidth) / static_cast<float>(a_outputHeight);
	a_consts.cameraFOV = Util::GetVerticalFOVRad();
	a_consts.cameraNear = Snapshot::Core().cameraNear;
	a_consts.cameraFar = Snapshot::Core().cameraFar;

	auto viewMatrix = Snapshot::FrameCamera::Main().GetCameraViewInverse().Transpose();
	auto cameraViewToClip = Snapshot::FrameCamera::Main().GetCameraProjUnjittered().Transpose();

	a_consts.cameraMotionIncluded = sl::Boolean::eTrue;
	a_consts.cameraPinholeOffset = { 0.f, 0.f };
	a_consts.cameraRight = { viewMatrix._11, viewMatrix._12, viewMatrix._13 };
	a_consts.cameraUp = { viewMatrix._21, viewMatrix._22, viewMatrix._23 };
	a_consts.cameraFwd = { viewMatrix._31, viewMatrix._32, viewMatrix._33 };
	const auto cameraPosAdjust = Snapshot::FrameCamera::Main().GetCameraPosAdjust();
	a_consts.cameraPos = *reinterpret_cast<const sl::float3*>(&cameraPosAdjust);
	a_consts.cameraViewToClip = *reinterpret_cast<const sl::float4x4*>(&cameraViewToClip);
	a_consts.depthInverted = sl::Boolean::eFalse;

	sl::recalculateCameraMatrices(a_consts);

	// Translate between the current and previous camera-relative origins before reprojection.
	// Streamline applies jitter separately, so both matrices remain unjittered.
	Matrix curVP = Snapshot::FrameCamera::Main().GetCameraViewProjUnjittered().Transpose();
	Matrix prevVP = Snapshot::FrameCamera::Main().GetCameraPreviousViewProjUnjittered().Transpose();
	const auto& posAdj = Snapshot::FrameCamera::Main().GetCameraPosAdjust();
	const auto& prevPosAdj = Snapshot::FrameCamera::Main().GetCameraPreviousPosAdjust();
	Matrix camDelta = Matrix::CreateTranslation(posAdj.x - prevPosAdj.x, posAdj.y - prevPosAdj.y, posAdj.z - prevPosAdj.z);
	// A camera cut or load (State::RequestHistoryReset, upstream #2354) resets the history.
	a_consts.reset = globals::state->ShouldResetHistory() ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	Matrix clipToPrevClip = curVP.Invert() * camDelta * prevVP;
	Matrix prevClipToClip = clipToPrevClip.Invert();

	// Matrix::Invert() yields NaN/inf when the source is singular, which the cached view-projection
	// is on transient frames -- a camera cut, a menu, the frame after a load. Feeding Streamline
	// valid constants every frame matters more than describing the motion of a frame whose
	// matrices do not exist yet, so substitute identity below rather than skipping the evaluate.
	const bool reprojectionFinite = cs_IsFiniteMatrix(clipToPrevClip) && cs_IsFiniteMatrix(prevClipToClip);
	if (!reprojectionFinite) {
		// Substitute identity -- "no camera motion this frame" -- rather than feeding Streamline NaN.
		// Deliberately NOT a temporal reset: a reset wipes the upscaler's history, and doing that on
		// a transient frame costs far more (visible shimmer on hair and grass, which carry the most
		// high-frequency detail) than one frame of slightly wrong reprojection.
		clipToPrevClip = Matrix::Identity;
		prevClipToClip = Matrix::Identity;
		{
			static uint32_t s_singularCount = 0u;
			static uint32_t s_nextReport = 1u;
			if (++s_singularCount >= s_nextReport) {
				s_nextReport = s_singularCount * 8u;
				logger::warn("[Streamline] singular camera reprojection matrices on {} frame(s) so far", s_singularCount);
			}
		}
	}
	a_consts.clipToPrevClip = *reinterpret_cast<const sl::float4x4*>(&clipToPrevClip);
	a_consts.prevClipToClip = *reinterpret_cast<const sl::float4x4*>(&prevClipToClip);

	a_consts.jitterOffset = { -a_jitterX, -a_jitterY };
	a_consts.mvecScale = { 1.0f, 1.0f };
	a_consts.motionVectors3D = sl::Boolean::eFalse;
	a_consts.motionVectorsInvalidValue = FLT_MIN;
	a_consts.orthographicProjection = sl::Boolean::eFalse;
	a_consts.motionVectorsDilated = sl::Boolean::eFalse;
	a_consts.motionVectorsJittered = sl::Boolean::eFalse;

	// Reject singular engine matrices before passing constants to Streamline.
	const bool matricesFinite = cs_IsFiniteMatrix(cameraViewToClip) &&
	                            cs_IsFiniteMatrix(clipToPrevClip) &&
	                            cs_IsFiniteMatrix(prevClipToClip);
	const bool basisFinite = std::isfinite(a_consts.cameraRight.x) &&
	                         std::isfinite(a_consts.cameraRight.y) &&
	                         std::isfinite(a_consts.cameraRight.z) &&
	                         std::isfinite(a_consts.cameraUp.x) &&
	                         std::isfinite(a_consts.cameraUp.y) &&
	                         std::isfinite(a_consts.cameraUp.z) &&
	                         std::isfinite(a_consts.cameraFwd.x) &&
	                         std::isfinite(a_consts.cameraFwd.y) &&
	                         std::isfinite(a_consts.cameraFwd.z);
	const bool scalarsFinite = std::isfinite(a_consts.cameraAspectRatio) &&
	                           std::isfinite(a_consts.cameraFOV) &&
	                           std::isfinite(a_consts.cameraNear) &&
	                           std::isfinite(a_consts.cameraFar);
	static bool s_cameraDataInvalid = false;
	if (!matricesFinite || !basisFinite || !scalarsFinite) {
		if (!s_cameraDataInvalid) {
			s_cameraDataInvalid = true;
			logger::warn("[Streamline] skipping evaluate: invalid camera constants "
				"(matrices={} basis={} scalars={} proj=[{},{},{},{},{},{}])",
				matricesFinite, basisFinite, scalarsFinite,
				cameraViewToClip._11, cameraViewToClip._22, cameraViewToClip._33,
				cameraViewToClip._34, cameraViewToClip._43, cameraViewToClip._44);
		}
		return false;
	}
	if (s_cameraDataInvalid) {
		s_cameraDataInvalid = false;
		logger::info("[Streamline] camera constants became valid; evaluations resumed");
	}

	return true;
}

static VkImageAspectFlags cs_ImageAspect(VkFormat a_format)
{
	switch (a_format) {
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return VK_IMAGE_ASPECT_DEPTH_BIT;
	default:
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}
}

struct cs_VulkanResultAttempt
{
	VkResult result = VK_ERROR_DEVICE_LOST;
	DWORD exceptionCode = 0;
};

struct cs_VulkanVoidAttempt
{
	DWORD exceptionCode = 0;
	bool completed = false;
};

struct cs_VulkanProcAttempt
{
	PFN_vkVoidFunction function = nullptr;
	DWORD exceptionCode = 0;
};

struct cs_GetVkImageAttempt
{
	bool succeeded = false;
	DWORD exceptionCode = 0;
};

static cs_GetVkImageAttempt cs_GetVkImageSEH(DXVKInterop* a_dxvk, ID3D11Resource* a_resource,
	VkImage* a_image, VkImageLayout* a_layout, VkImageCreateInfo* a_info) noexcept
{
	cs_GetVkImageAttempt attempt{};
	__try {
		attempt.succeeded = a_dxvk &&
			a_dxvk->GetVkImage(a_resource, a_image, a_layout, a_info);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		attempt.exceptionCode = GetExceptionCode();
	}
	return attempt;
}

static cs_VulkanProcAttempt cs_GetDeviceProcAddrSEH(
	PFN_vkGetDeviceProcAddr a_getProcAddr, VkDevice a_device, const char* a_name) noexcept
{
	cs_VulkanProcAttempt attempt{};
	__try {
		if (a_getProcAddr)
			attempt.function = a_getProcAddr(a_device, a_name);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		attempt.exceptionCode = GetExceptionCode();
	}
	return attempt;
}

static cs_VulkanResultAttempt cs_CreateImageViewSEH(PFN_vkCreateImageView a_createImageView,
	VkDevice a_device, const VkImageViewCreateInfo* a_createInfo, VkImageView* a_view) noexcept
{
	cs_VulkanResultAttempt attempt{};
	__try {
		if (a_createImageView)
			attempt.result = a_createImageView(a_device, a_createInfo, nullptr, a_view);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		attempt.exceptionCode = GetExceptionCode();
	}
	return attempt;
}

static cs_VulkanVoidAttempt cs_DestroyImageViewSEH(PFN_vkDestroyImageView a_destroyImageView,
	VkDevice a_device, VkImageView a_view) noexcept
{
	cs_VulkanVoidAttempt attempt{};
	__try {
		if (a_destroyImageView && a_view != VK_NULL_HANDLE)
			a_destroyImageView(a_device, a_view, nullptr);
		attempt.completed = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		attempt.exceptionCode = GetExceptionCode();
	}
	return attempt;
}

static cs_VulkanVoidAttempt cs_PipelineBarrierSEH(VkCommandBuffer a_commandBuffer,
	const VkImageMemoryBarrier* a_barrier) noexcept
{
	cs_VulkanVoidAttempt attempt{};
	__try {
		vkCmdPipelineBarrier(a_commandBuffer,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
			0, nullptr, 0, nullptr, 1, a_barrier);
		attempt.completed = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		attempt.exceptionCode = GetExceptionCode();
	}
	return attempt;
}

// Streamline's Vulkan backend requires a matching VkImageView for every resource.
static bool cs_WrapInteropImage(DXVKInterop* a_dxvk, VkDevice a_device, PFN_vkCreateImageView a_createView,
	ID3D11Resource* a_res, sl::Resource& a_out, sl::SubresourceRange& a_subresource,
	VkImageView& a_outView, bool& a_terminalFault)
{
	a_outView = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	const cs_GetVkImageAttempt imageAttempt =
		cs_GetVkImageSEH(a_dxvk, a_res, &image, &layout, &info);
	if (imageAttempt.exceptionCode) {
		a_terminalFault = true;
		logger::error("[Streamline] DXVK image interop faulted (SEH {:#x})",
			imageAttempt.exceptionCode);
		return false;
	}
	if (!imageAttempt.succeeded || image == VK_NULL_HANDLE)
		return false;
	VkImageView view = VK_NULL_HANDLE;
	if (!a_createView)
		return false;
	VkImageViewCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	ci.image = image;
	ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ci.format = info.format;
	ci.subresourceRange.aspectMask = cs_ImageAspect(info.format);
	ci.subresourceRange.levelCount = 1;
	ci.subresourceRange.layerCount = 1;
	const cs_VulkanResultAttempt createAttempt =
		cs_CreateImageViewSEH(a_createView, a_device, &ci, &view);
	if (createAttempt.exceptionCode || createAttempt.result != VK_SUCCESS || view == VK_NULL_HANDLE) {
		if (createAttempt.exceptionCode || view != VK_NULL_HANDLE) {
			a_terminalFault = true;
		}
		view = VK_NULL_HANDLE;
		logger::error("[Streamline] Vulkan image-view creation failed (result {}, SEH {:#x})",
			static_cast<int>(createAttempt.result), createAttempt.exceptionCode);
		a_outView = VK_NULL_HANDLE;
		return false;
	}
	a_outView = view;
	a_out = sl::Resource{ sl::ResourceType::eTex2d, image, nullptr, view, static_cast<uint32_t>(layout) };
	a_out.width = info.extent.width;
	a_out.height = info.extent.height;
	a_out.nativeFormat = static_cast<uint32_t>(info.format);
	a_out.mipLevels = info.mipLevels;
	a_out.arrayLayers = info.arrayLayers;
	a_out.usage = static_cast<uint32_t>(info.usage);
	a_out.flags = static_cast<uint32_t>(info.flags);
	a_subresource.aspectMask = ci.subresourceRange.aspectMask;
	a_subresource.baseMipLevel = 0;
	a_subresource.levelCount = 1;
	a_subresource.baseArrayLayer = 0;
	a_subresource.layerCount = 1;
	a_out.next = &a_subresource;
	return true;
}

// a_dxvk/a_resources/a_resourceCount only ever fed the removed quarantine path; they are kept
// so the call sites still read as "these views belong to these resources".
static bool cs_DestroyViews([[maybe_unused]] DXVKInterop* a_dxvk, VkDevice a_device,
	PFN_vkDestroyImageView a_destroyImageView, VkImageView* a_views, uint32_t a_count,
	[[maybe_unused]] ID3D11Resource* const* a_resources = nullptr,
	[[maybe_unused]] uint32_t a_resourceCount = 0)
{
	if (!a_destroyImageView)
		return false;
	for (uint32_t i = 0; i < a_count; ++i) {
		if (a_views[i] == VK_NULL_HANDLE)
			continue;
		const cs_VulkanVoidAttempt destroyAttempt =
			cs_DestroyImageViewSEH(a_destroyImageView, a_device, a_views[i]);
		if (!destroyAttempt.completed) {
			a_views[i] = VK_NULL_HANDLE;
			return false;
		}
		a_views[i] = VK_NULL_HANDLE;
	}
	return true;
}

static bool cs_BarrierUpscalerOutput(VkCommandBuffer a_commandBuffer, const sl::Resource& a_output)
{
	VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	barrier.oldLayout = static_cast<VkImageLayout>(a_output.state);
	barrier.newLayout = static_cast<VkImageLayout>(a_output.state);
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = static_cast<VkImage>(a_output.native);
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = a_output.mipLevels;
	barrier.subresourceRange.layerCount = a_output.arrayLayers;
	const cs_VulkanVoidAttempt barrierAttempt = cs_PipelineBarrierSEH(a_commandBuffer, &barrier);
	return barrierAttempt.completed;
}

static bool cs_SubmitPresentTags(DXVKInterop* a_dxvk, sl::FrameToken& a_token,
	const sl::ViewportHandle& a_viewport, const sl::ResourceTag* a_tags, uint32_t a_tagCount,
	const VkImageView* a_views, uint32_t a_viewCount,
	ID3D11Resource* const* a_resources, uint32_t a_resourceCount, sl::Result& a_tagResult,
	bool& a_lifetimesRetained)
{
	a_lifetimesRetained = false;
	auto transaction = a_dxvk->BeginFrameCommandBuffer();
	if (!transaction) {
		logger::error("[Streamline] present tags: could not acquire an interop command buffer");
		return false;
	}

	a_tagResult = cs_SetTagForFrame(
		a_token, a_viewport, a_tags, a_tagCount, transaction.GetCommandBuffer());
	if (a_tagResult != sl::Result::eOk) {
		logger::error("[Streamline] present tags: slSetTagForFrame failed (result {})",
			static_cast<int>(a_tagResult));
		return false;
	}
	if (!a_dxvk->SubmitFrameCommandBuffer(transaction)) {
		logger::error("[Streamline] present tags: interop submit failed");
		if (transaction.SubmissionMayBeInFlight()) {
			a_dxvk->QueueViewsForDeferredDelete(transaction, a_views, a_viewCount);
			a_dxvk->QueueResourcesForDeferredRelease(transaction, a_resources, a_resourceCount);
			a_lifetimesRetained = true;
		}
		return false;
	}

	a_dxvk->QueueViewsForDeferredDelete(transaction, a_views, a_viewCount);
	a_dxvk->QueueResourcesForDeferredRelease(transaction, a_resources, a_resourceCount);
	return true;
}

static bool cs_CanReleaseFailedFSRFrame(DXVKInterop* a_dxvk,
	DXVKInterop::CommandTransaction& a_transaction, const sl::ViewportHandle& a_viewport,
	const VkImageView* a_views, uint32_t a_viewCount,
	ID3D11Resource* const* a_resources, uint32_t a_resourceCount)
{
	if (!a_transaction.SubmissionMayBeInFlight()) {
		const sl::Result discardResult = cs_DiscardFSRFrameGenerationPreparedFrame(a_viewport);
		if (discardResult == sl::Result::eOk || discardResult == sl::Result::eErrorInvalidState)
			return true;
		logger::error("[Streamline] failed to discard a partially accepted FSR-FG frame (result {})",
			static_cast<int>(discardResult));
	}

	a_dxvk->QueueResourcesForDeferredRelease(a_transaction, a_resources, a_resourceCount);
	a_dxvk->QueueViewsForDeferredDelete(a_transaction, a_views, a_viewCount);
	return false;
}


/**
 * @brief Sets one evaluate's SDK options (mode, output size, pre-exposure), built from its frame's values. Called right before the
 * evaluate records, so a later frame's options never replace this frame's first.
 */
using cs_SetOptions = std::function<sl::Result()>;

/**
 * @brief The evaluate as the post stage's pass (native mode, and guest mode since the post stage runs there): the frame's constants
 * now, then the tags and the evaluate as the evaluate pass at the Upscale anchor (ORGUpscaling::QueueEvaluate), which records them on
 * its own command list in the post stage, on the thread that records it (in guest, DXVK's command buffer inside the stage's block). The
 * images are the pass's: kMAIN in, the upscaled texture out, kMAIN's depth and the motion vectors, each in the common state. The
 * upscaled output's copy into kMAIN is the same anchor's next pass. The result is known only when the pass records: an evaluate the
 * SDK refuses is logged there, and this frame's output is the previous one.
 */
static sl::Result cs_EvaluateFeatureAsPass(sl::Feature a_feature, const sl::ViewportHandle& a_viewport,
	ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut, ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_outputWidth, uint32_t a_outputHeight,
	float a_jitterX, float a_jitterY, ID3D11Resource* a_hudlessColor, bool* a_outputReady, bool* a_skipped, cs_SetOptions a_options)
{
	using RenderGraph::Engine::Native::ExternalImageInfo;
	const auto skip = [&] {
		if (a_skipped)
			*a_skipped = true;
		return sl::Result::eOk;
	};
	sl::FrameToken* token = RenderFrameToken();
	if (!token)
		return skip();
	static uint32_t s_evalFrameByVp[2] = { UINT32_MAX, UINT32_MAX };
	const uint32_t vpId = a_viewport;
	const uint32_t frame = g_sl.renderFrameId.load(std::memory_order_acquire);
	if (vpId < 2 && s_evalFrameByVp[vpId] == frame)
		return skip();
	// The pass's images are kMAIN, the upscaled texture, kMAIN's depth and the motion vectors: the only evaluate the post stage makes.
	if (!a_colorIn || !a_colorOut || !a_depth || !a_motionVectors || a_hudlessColor) {
		static std::once_flag logged;
		std::call_once(logged, [] { logger::error("[Streamline] the evaluate pass takes only colour, depth and motion vectors without a hudless colour; skipped (logged once)"); });
		return skip();
	}
	sl::Constants consts;
	if (!cs_BuildConstants(consts, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY))
		return skip();
	if (const sl::Result constantsRes = g_sl.slSetConstants(consts, *token, a_viewport); constantsRes != sl::Result::eOk) {
		logger::error("[Streamline] slSetConstants failed for viewport {} (result {})", vpId, static_cast<int>(constantsRes));
		return constantsRes;
	}
	if (vpId == 0)
		g_viewport0ConstantsFrame.store(frame, std::memory_order_release);

	const bool d3d12 = RenderGraph::Backend::IsD3D12();
	const sl::ViewportHandle viewport = a_viewport;
	const char* label = a_feature == sl::kFeatureDLSS ? "Streamline DLSS" : a_feature == sl::kFeatureXeSS ? "Streamline XeSS" : "Streamline FSR";
	// Runs later, in the evaluate pass's Record: everything it uses is captured by value. The token stays valid: Streamline keeps a
	// ring of them, and the pass records within the frames in flight. The options too are this frame's, set where it records.
	auto record = [=](void* a_list, std::span<const ExternalImageInfo> a_infos) {
		if (a_options) {
			if (const sl::Result optionsRes = a_options(); optionsRes != sl::Result::eOk) {
				logger::error("[Streamline] the options of feature {} viewport {} failed (result {})", static_cast<uint32_t>(a_feature), static_cast<uint32_t>(viewport),
					static_cast<int>(optionsRes));
				return false;
			}
		}
		std::array<sl::Resource, 4> resources{};
		std::array<sl::SubresourceRange, 4> ranges{};
		for (std::size_t i = 0; i < a_infos.size() && i < resources.size(); ++i) {
			const auto& info = a_infos[i];
			if (d3d12) {
				resources[i] = sl::Resource{ sl::ResourceType::eTex2d, info.native, static_cast<uint32_t>(info.state) };
			} else {
				resources[i] = sl::Resource{ sl::ResourceType::eTex2d, info.native, nullptr, reinterpret_cast<void*>(info.view), static_cast<uint32_t>(info.state) };
				resources[i].width = info.width;
				resources[i].height = info.height;
				resources[i].nativeFormat = info.format;
				resources[i].mipLevels = info.mips;
				resources[i].arrayLayers = info.layers;
				resources[i].usage = info.usage;
				ranges[i].aspectMask = cs_ImageAspect(static_cast<VkFormat>(info.format));
				ranges[i].baseMipLevel = 0;
				ranges[i].levelCount = info.mips;
				ranges[i].baseArrayLayer = 0;
				ranges[i].layerCount = info.layers;
				resources[i].next = &ranges[i];
			}
		}
		sl::Extent renderExtent{ 0, 0, a_renderWidth, a_renderHeight };
		sl::Extent outputExtent{ 0, 0, a_outputWidth, a_outputHeight };
		std::array<sl::ResourceTag, 4> tags{
			sl::ResourceTag{ &resources[0], sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent },
			sl::ResourceTag{ &resources[1], sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outputExtent },
			sl::ResourceTag{ &resources[2], sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent },
			sl::ResourceTag{ &resources[3], sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent }
		};
		auto* commandBuffer = static_cast<sl::CommandBuffer*>(a_list);
		if (const sl::Result tagRes = g_sl.slSetTagForFrame(*token, viewport, tags.data(), static_cast<uint32_t>(tags.size()), commandBuffer); tagRes != sl::Result::eOk) {
			logger::error("[Streamline] slSetTagForFrame failed for feature {} viewport {} (result {})", static_cast<uint32_t>(a_feature), static_cast<uint32_t>(viewport),
				static_cast<int>(tagRes));
			return false;
		}
		const sl::BaseStructure* inputs[] = { &viewport };
		const sl::Result evalRes = g_sl.slEvaluateFeature(a_feature, *token, inputs, static_cast<uint32_t>(std::size(inputs)), commandBuffer);
		if (evalRes != sl::Result::eOk && evalRes != sl::Result::eWarnOutOfVRAM) {
			logger::error("[Streamline] slEvaluateFeature failed for feature {} viewport {} (result {})", static_cast<uint32_t>(a_feature), static_cast<uint32_t>(viewport),
				static_cast<int>(evalRes));
			return false;
		}
		return true;
	};
	if (!ORGUpscaling::Get().QueueEvaluate(label, std::move(record))) {
		static std::once_flag logged;
		std::call_once(logged, [label] { logger::error("[Streamline] {}: the evaluate pass cannot run (Upscaling's graph path or its inputs are missing); skipped (logged once)", label); });
		return sl::Result::eErrorNotInitialized;
	}
	if (a_outputReady)
		*a_outputReady = true;
	if (vpId < 2)
		s_evalFrameByVp[vpId] = frame;
	return sl::Result::eOk;
}

/**
 * @brief FSR-FG's inputs as the UI stage's pass, after the hudless copy (ORGUpscaling::QueueFrameGenInputs): the frame's constants
 * now, then the tags and the prepare on the pass's own command list when it records. The images are kMAIN's depth (or its copy),
 * the motion vectors and the hudless, each in the common state, where the hudless stays until the present reads it. Everything the
 * recording uses is captured by value, so a replayed recording records the same prepare again.
 */
static sl::Result cs_RecordFrameGenInputs(uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_outputWidth, uint32_t a_outputHeight,
	float a_jitterX, float a_jitterY, bool a_depthCopy, bool* a_queued)
{
	using RenderGraph::Engine::Native::ExternalImageInfo;
	*a_queued = false;
	sl::FrameToken* token = RenderFrameToken();
	if (!token)
		return sl::Result::eOk;
	// One prepare a frame.
	static uint32_t s_queuedFrame = UINT32_MAX;
	const uint32_t frame = g_sl.renderFrameId.load(std::memory_order_acquire);
	if (s_queuedFrame == frame)
		return sl::Result::eOk;
	// Frame generation's own viewport, apart from the upscalers' tags and constants.
	const sl::ViewportHandle viewport{ 1 };
	sl::Constants consts;
	if (!cs_BuildConstants(consts, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY))
		return sl::Result::eOk;
	if (const sl::Result constantsRes = g_sl.slSetConstants(consts, *token, viewport); constantsRes != sl::Result::eOk) {
		logger::error("[Streamline] slSetConstants failed for viewport 1 (result {})", static_cast<int>(constantsRes));
		return constantsRes;
	}

	const bool d3d12 = RenderGraph::Backend::IsD3D12();
	auto record = [=](void* a_list, std::span<const ExternalImageInfo> a_infos) {
		std::array<sl::Resource, 3> resources{};
		std::array<sl::SubresourceRange, 3> ranges{};
		for (std::size_t i = 0; i < a_infos.size() && i < resources.size(); ++i) {
			const auto& info = a_infos[i];
			if (d3d12) {
				resources[i] = sl::Resource{ sl::ResourceType::eTex2d, info.native, static_cast<uint32_t>(info.state) };
			} else {
				resources[i] = sl::Resource{ sl::ResourceType::eTex2d, info.native, nullptr, reinterpret_cast<void*>(info.view), static_cast<uint32_t>(info.state) };
				resources[i].width = info.width;
				resources[i].height = info.height;
				resources[i].nativeFormat = info.format;
				resources[i].mipLevels = info.mips;
				resources[i].arrayLayers = info.layers;
				resources[i].usage = info.usage;
				ranges[i].aspectMask = cs_ImageAspect(static_cast<VkFormat>(info.format));
				ranges[i].baseMipLevel = 0;
				ranges[i].levelCount = info.mips;
				ranges[i].baseArrayLayer = 0;
				ranges[i].layerCount = info.layers;
				resources[i].next = &ranges[i];
			}
		}
		sl::Extent renderExtent{ 0, 0, a_renderWidth, a_renderHeight };
		sl::Extent outputExtent{ 0, 0, a_outputWidth, a_outputHeight };
		std::array<sl::ResourceTag, 3> tags{
			sl::ResourceTag{ &resources[0], sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent },
			sl::ResourceTag{ &resources[1], sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent },
			sl::ResourceTag{ &resources[2], sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent }
		};
		auto* commandBuffer = static_cast<sl::CommandBuffer*>(a_list);
		if (const sl::Result tagRes = g_sl.slSetTagForFrame(*token, viewport, tags.data(), static_cast<uint32_t>(tags.size()), commandBuffer); tagRes != sl::Result::eOk) {
			logger::error("[Streamline] slSetTagForFrame failed for FSR-FG's inputs (result {})", static_cast<int>(tagRes));
			return false;
		}
		const sl::BaseStructure* inputs[] = { &viewport };
		const sl::Result evalRes = g_sl.slEvaluateFeature(sl::kFeatureFSR_G, *token, inputs, static_cast<uint32_t>(std::size(inputs)), commandBuffer);
		if (evalRes != sl::Result::eOk && evalRes != sl::Result::eWarnOutOfVRAM) {
			logger::error("[Streamline] slEvaluateFeature failed for FSR-FG's inputs (result {})", static_cast<int>(evalRes));
			return false;
		}
		return true;
	};
	if (!ORGUpscaling::Get().QueueFrameGenInputs("Streamline FSR-FG inputs", std::move(record), a_depthCopy))
		return sl::Result::eOk;
	static std::once_flag logged;
	std::call_once(logged, [] { logger::info("[Streamline] FSR-FG's inputs are the UI stage's pass"); });
	*a_queued = true;
	s_queuedFrame = frame;
	return sl::Result::eOk;
}

/**
 * @brief DLSS-G reads the options' viewport, where the upscalers set the frame's constants. Without an upscaler this sets them,
 * once a frame. False when the frame has none.
 */
static bool cs_EnsureDLSSGConstants(uint32_t a_displayWidth, uint32_t a_displayHeight, float a_jitterX, float a_jitterY)
{
	sl::FrameToken* token = RenderFrameToken();
	if (!token)
		return false;
	const uint32_t frame = g_sl.renderFrameId.load(std::memory_order_acquire);
	if (g_viewport0ConstantsFrame.load(std::memory_order_acquire) == frame)
		return true;
	sl::Constants consts;
	if (!cs_BuildConstants(consts, a_displayWidth, a_displayHeight, a_jitterX, a_jitterY))
		return false;
	if (const sl::Result constantsRes = g_sl.slSetConstants(consts, *token, g_sl.viewport); constantsRes != sl::Result::eOk) {
		logger::error("[Streamline] slSetConstants failed for DLSS-G's viewport (result {})", static_cast<int>(constantsRes));
		return false;
	}
	g_viewport0ConstantsFrame.store(frame, std::memory_order_release);
	return true;
}

/**
 * @brief DLSS-G's tags as the UI stage's pass, after the hudless copy (ORGUpscaling::QueueFrameGenInputs): the depth, motion
 * vector and hudless tags on the pass's command list. Each is tagged valid only now, so Streamline copies it in the state the
 * pass declares, and DLSS-G reads the copies at the present. DLSS-G has no evaluate. Everything the recording uses is captured
 * by value, so a replayed recording tags again.
 */
static sl::Result cs_RecordDLSSGInputs(uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight,
	bool a_depthCopy, bool* a_queued)
{
	using RenderGraph::Engine::Native::ExternalImageInfo;
	*a_queued = false;
	sl::FrameToken* token = RenderFrameToken();
	if (!token)
		return sl::Result::eOk;
	static uint32_t s_queuedFrame = UINT32_MAX;
	const uint32_t frame = g_sl.renderFrameId.load(std::memory_order_acquire);
	if (s_queuedFrame == frame)
		return sl::Result::eOk;
	const sl::ViewportHandle viewport = g_sl.viewport;

	const bool d3d12 = RenderGraph::Backend::IsD3D12();
	auto record = [=](void* a_list, std::span<const ExternalImageInfo> a_infos) {
		std::array<sl::Resource, 3> resources{};
		std::array<sl::SubresourceRange, 3> ranges{};
		for (std::size_t i = 0; i < a_infos.size() && i < resources.size(); ++i) {
			const auto& info = a_infos[i];
			if (d3d12) {
				resources[i] = sl::Resource{ sl::ResourceType::eTex2d, info.native, static_cast<uint32_t>(info.state) };
			} else {
				resources[i] = sl::Resource{ sl::ResourceType::eTex2d, info.native, nullptr, reinterpret_cast<void*>(info.view), static_cast<uint32_t>(info.state) };
				resources[i].width = info.width;
				resources[i].height = info.height;
				resources[i].nativeFormat = info.format;
				resources[i].mipLevels = info.mips;
				resources[i].arrayLayers = info.layers;
				resources[i].usage = info.usage;
				ranges[i].aspectMask = cs_ImageAspect(static_cast<VkFormat>(info.format));
				ranges[i].baseMipLevel = 0;
				ranges[i].levelCount = info.mips;
				ranges[i].baseArrayLayer = 0;
				ranges[i].layerCount = info.layers;
				resources[i].next = &ranges[i];
			}
		}
		sl::Extent renderExtent{ 0, 0, a_renderWidth, a_renderHeight };
		sl::Extent displayExtent{ 0, 0, a_displayWidth, a_displayHeight };
		std::array<sl::ResourceTag, 3> tags{
			sl::ResourceTag{ &resources[0], sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent },
			sl::ResourceTag{ &resources[1], sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent },
			sl::ResourceTag{ &resources[2], sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &displayExtent }
		};
		auto* commandBuffer = static_cast<sl::CommandBuffer*>(a_list);
		if (const sl::Result tagRes = g_sl.slSetTagForFrame(*token, viewport, tags.data(), static_cast<uint32_t>(tags.size()), commandBuffer); tagRes != sl::Result::eOk) {
			logger::error("[Streamline] slSetTagForFrame failed for DLSS-G's inputs (result {})", static_cast<int>(tagRes));
			return false;
		}
		return true;
	};
	if (!ORGUpscaling::Get().QueueFrameGenInputs("Streamline DLSS-G inputs", std::move(record), a_depthCopy))
		return sl::Result::eOk;
	static std::once_flag logged;
	std::call_once(logged, [] { logger::info("[Streamline] DLSS-G's inputs are the UI stage's pass"); });
	*a_queued = true;
	s_queuedFrame = frame;
	return sl::Result::eOk;
}

static sl::Result cs_EvaluateFeatureCore(sl::Feature a_feature, const sl::ViewportHandle& a_viewport,
	ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut, ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_outputWidth, uint32_t a_outputHeight,
	float a_jitterX, float a_jitterY, ID3D11Resource* a_hudlessColor = nullptr,
	bool* a_outputReady = nullptr, bool* a_skipped = nullptr, const cs_SetOptions& a_options = {})
{
	if (a_outputReady)
		*a_outputReady = false;
	if (a_skipped)
		*a_skipped = false;
	// The upscalers' evaluate is the post stage's pass wherever that stage runs (the recipe); frame generation's stays in the ring.
	if (RenderGraph::Engine::Native::Active() || (a_feature != sl::kFeatureFSR_G && ORGUpscaling::Get().EvaluatesInStage()))
		return cs_EvaluateFeatureAsPass(a_feature, a_viewport, a_colorIn, a_colorOut, a_depth, a_motionVectors, a_renderWidth, a_renderHeight, a_outputWidth,
			a_outputHeight, a_jitterX, a_jitterY, a_hudlessColor, a_outputReady, a_skipped, a_options);
	// The ring records now, so its options are set now.
	if (a_options) {
		if (const sl::Result optionsRes = a_options(); optionsRes != sl::Result::eOk) {
			logger::error("[Streamline] the options of feature {} viewport {} failed (result {})", static_cast<uint32_t>(a_feature),
				static_cast<uint32_t>(a_viewport), static_cast<int>(optionsRes));
			return optionsRes;
		}
	}

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk)
		return sl::Result::eErrorNotInitialized;

	sl::FrameToken* token = RenderFrameToken();
	if (!token) {
		if (a_skipped)
			*a_skipped = true;
		return sl::Result::eOk;
	}

	// Pair constants and evaluation once per frame and viewport.
	static uint32_t s_evalFrameByVp[2] = { UINT32_MAX, UINT32_MAX };
	static uint32_t s_constFrameByVp[2] = { UINT32_MAX, UINT32_MAX };
	const uint32_t vpId = a_viewport;
	if (vpId < 2) {
		if (s_evalFrameByVp[vpId] == g_sl.renderFrameId.load(std::memory_order_acquire)) {
			if (a_skipped)
				*a_skipped = true;
			return sl::Result::eOk;
		}
		if (s_constFrameByVp[vpId] != g_sl.renderFrameId.load(std::memory_order_acquire)) {
			sl::Constants consts;
			if (!cs_BuildConstants(consts, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY)) {
				if (a_skipped)
					*a_skipped = true;
				return sl::Result::eOk;
			}
			const sl::Result constantsRes = g_sl.slSetConstants(consts, *token, a_viewport);
			if (constantsRes != sl::Result::eOk) {
				logger::error("[Streamline] slSetConstants failed for viewport {} (result {})",
					vpId, static_cast<int>(constantsRes));
				return constantsRes;
			}
			s_constFrameByVp[vpId] = g_sl.renderFrameId.load(std::memory_order_acquire);
			if (vpId == 0)
				g_viewport0ConstantsFrame.store(s_constFrameByVp[vpId], std::memory_order_release);
		}
	} else {
		sl::Constants consts;
		if (!cs_BuildConstants(consts, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY)) {
			if (a_skipped)
				*a_skipped = true;
			return sl::Result::eOk;
		}
		const sl::Result constantsRes = g_sl.slSetConstants(consts, *token, a_viewport);
		if (constantsRes != sl::Result::eOk)
			return constantsRes;
	}

	VkDevice vkDevice = dxvk->GetDevice();
	const cs_VulkanProcAttempt createProcAttempt = cs_GetDeviceProcAddrSEH(
		dxvk->GetDeviceProcAddr(), vkDevice, "vkCreateImageView");
	const cs_VulkanProcAttempt destroyProcAttempt = cs_GetDeviceProcAddrSEH(
		dxvk->GetDeviceProcAddr(), vkDevice, "vkDestroyImageView");
	if (createProcAttempt.exceptionCode || destroyProcAttempt.exceptionCode) {
		return sl::Result::eErrorExceptionHandler;
	}
	auto vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(createProcAttempt.function);
	auto vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(destroyProcAttempt.function);
	if (!vkCreateImageView || !vkDestroyImageView)
		return sl::Result::eErrorNotInitialized;
	ID3D11Resource* resources[] = {
		a_colorIn, a_colorOut, a_depth, a_motionVectors, a_hudlessColor
	};
	VkImageView views[5] = {};
	sl::SubresourceRange subresources[5]{};
	int nv = 0;
	int nr = 0;
	bool viewCreationTerminalFault = false;
	const auto wrap = [&](ID3D11Resource* a_res, sl::Resource& a_out) -> bool {
		VkImageView v = VK_NULL_HANDLE;
		if (nr >= static_cast<int>(std::size(subresources)))
			return false;
		const bool wrapped = cs_WrapInteropImage(
			dxvk, vkDevice, vkCreateImageView, a_res, a_out, subresources[nr], v,
			viewCreationTerminalFault);
		if (v != VK_NULL_HANDLE && nv < static_cast<int>(std::size(views)))
			views[nv++] = v;
		if (!wrapped)
			return false;
		++nr;
		return true;
	};

	const bool haveColor = (a_colorIn && a_colorOut);
	const bool haveHudless = (a_hudlessColor != nullptr);
	sl::Resource colorInRes{}, colorOutRes{}, depthRes{}, mvecRes{}, hudlessRes{};
	bool ok = wrap(a_depth, depthRes) &&
	          wrap(a_motionVectors, mvecRes);
	if (ok && haveColor)
		ok = wrap(a_colorIn, colorInRes) &&
		     wrap(a_colorOut, colorOutRes);
	if (ok && haveHudless)
		ok = wrap(a_hudlessColor, hudlessRes);
	if (!ok) {
		if (viewCreationTerminalFault) {
			// The destroy entry point already faulted on these handles; calling it again would fault
			// again, so abandon the views rather than retrying the destroy.
			std::fill(std::begin(views), std::end(views), VK_NULL_HANDLE);
		} else {
			cs_DestroyViews(dxvk, vkDevice, vkDestroyImageView, views, static_cast<uint32_t>(nv),
				resources, static_cast<uint32_t>(std::size(resources)));
		}
		return sl::Result::eErrorMissingInputParameter;
	}

	sl::Extent renderExtent{ 0, 0, a_renderWidth, a_renderHeight };
	sl::Extent outputExtent{ 0, 0, a_outputWidth, a_outputHeight };
	sl::ResourceTag tags[5];
	uint32_t nt = 0;
	if (haveColor) {
		tags[nt++] = sl::ResourceTag{ &colorInRes, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent };
		tags[nt++] = sl::ResourceTag{ &colorOutRes, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outputExtent };
	}
	const auto inputLifecycle = haveColor ?
		sl::ResourceLifecycle::eValidUntilEvaluate : sl::ResourceLifecycle::eOnlyValidNow;
	tags[nt++] = sl::ResourceTag{ &depthRes, sl::kBufferTypeDepth, inputLifecycle, &renderExtent };
	tags[nt++] = sl::ResourceTag{ &mvecRes, sl::kBufferTypeMotionVectors, inputLifecycle, &renderExtent };
	if (haveHudless)
		tags[nt++] = sl::ResourceTag{ &hudlessRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent };

	sl::Result evalRes = sl::Result::eErrorNotInitialized;
	// Timed inside the buffer: a D3D11 timer around it would straddle the submission (DXVKInterop.h).
	auto transaction = dxvk->BeginFrameCommandBuffer(
		a_feature == sl::kFeatureFSR_G ? nullptr : "Upscaling::Upscale");
	if (transaction) {
		const VkCommandBuffer cmd = transaction.GetCommandBuffer();
		const sl::Result tagRes = cs_SetTagForFrame(*token, a_viewport, tags, nt, cmd);
		if (tagRes != sl::Result::eOk) {
			logger::error("[Streamline] slSetTagForFrame failed for feature {} viewport {} (result {})",
				static_cast<uint32_t>(a_feature), vpId, static_cast<int>(tagRes));
			const bool canRelease = a_feature != sl::kFeatureFSR_G ||
				cs_CanReleaseFailedFSRFrame(dxvk, transaction, a_viewport,
					views, static_cast<uint32_t>(nv), resources, static_cast<uint32_t>(std::size(resources)));
			if (canRelease)
				cs_DestroyViews(dxvk, vkDevice, vkDestroyImageView, views,
					static_cast<uint32_t>(nv), resources,
					static_cast<uint32_t>(std::size(resources)));
			return tagRes;
		}

		evalRes = cs_EvaluateFeature(a_feature, *token, a_viewport, cmd);
		if (evalRes != sl::Result::eOk) {
			const bool canRelease = a_feature != sl::kFeatureFSR_G ||
				cs_CanReleaseFailedFSRFrame(dxvk, transaction, a_viewport,
					views, static_cast<uint32_t>(nv), resources, static_cast<uint32_t>(std::size(resources)));
			if (canRelease)
				cs_DestroyViews(dxvk, vkDevice, vkDestroyImageView, views,
					static_cast<uint32_t>(nv), resources,
					static_cast<uint32_t>(std::size(resources)));
			return evalRes;
		}

		if (haveColor && !cs_BarrierUpscalerOutput(cmd, colorOutRes)) {
			// The destroy entry point already faulted on these handles; calling it again would fault
			// again, so abandon the views rather than retrying the destroy.
			std::fill(std::begin(views), std::end(views), VK_NULL_HANDLE);
			return sl::Result::eErrorExceptionHandler;
		}

		if (dxvk->SubmitFrameCommandBuffer(transaction)) {
			dxvk->QueueResourcesForDeferredRelease(
				transaction, resources, static_cast<uint32_t>(std::size(resources)));
			dxvk->QueueViewsForDeferredDelete(transaction, views, static_cast<uint32_t>(nv));
			if (a_outputReady)
				*a_outputReady = true;
			if (vpId < 2)
				s_evalFrameByVp[vpId] = g_sl.renderFrameId.load(std::memory_order_acquire);
		} else {
			if (a_feature == sl::kFeatureFSR_G) {
				const bool canRelease = cs_CanReleaseFailedFSRFrame(dxvk, transaction, a_viewport,
					views, static_cast<uint32_t>(nv), resources, static_cast<uint32_t>(std::size(resources)));
				if (canRelease)
					cs_DestroyViews(dxvk, vkDevice, vkDestroyImageView, views,
						static_cast<uint32_t>(nv), resources,
						static_cast<uint32_t>(std::size(resources)));
			} else if (transaction.SubmissionMayBeInFlight()) {
				dxvk->QueueResourcesForDeferredRelease(
					transaction, resources, static_cast<uint32_t>(std::size(resources)));
				dxvk->QueueViewsForDeferredDelete(transaction, views, static_cast<uint32_t>(nv));
			} else {
				cs_DestroyViews(dxvk, vkDevice, vkDestroyImageView, views,
					static_cast<uint32_t>(nv), resources,
					static_cast<uint32_t>(std::size(resources)));
			}
			evalRes = sl::Result::eErrorExceptionHandler;
		}
	} else {
		cs_DestroyViews(dxvk, vkDevice, vkDestroyImageView, views,
			static_cast<uint32_t>(nv), resources,
			static_cast<uint32_t>(std::size(resources)));
		if (a_skipped)
			*a_skipped = true;
		evalRes = sl::Result::eOk;
	}
	return evalRes;
}

static Streamline::EvaluationResult cs_ClassifyEvaluation(
	sl::Result a_result, bool a_outputReady, bool a_skipped)
{
	if (a_result != sl::Result::eOk)
		return Streamline::EvaluationResult::kFailed;
	if (a_outputReady)
		return Streamline::EvaluationResult::kReady;
	return a_skipped ? Streamline::EvaluationResult::kSkipped : Streamline::EvaluationResult::kFailed;
}

Streamline::EvaluationResult Streamline::EvaluateDLSS(ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut,
	ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_outputWidth, uint32_t a_outputHeight,
	uint32_t a_qualityMode,
	float a_jitterX, float a_jitterY)
{
	bool outputReady = false;
	bool evaluationSkipped = false;
	EvaluationResult result = EvaluationResult::kFailed;
	if (!initialized || !featureDLSS)
		return result;
	if (!a_colorIn || !a_colorOut || !a_depth || !a_motionVectors)
		return result;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!native && !dxvk->CommandResourcesReady())
		return result;

	sl::DLSSMode dlssMode = sl::DLSSMode::eMaxQuality;
	switch (a_qualityMode) {
	case 0:
		dlssMode = sl::DLSSMode::eDLAA;
		break;
	case 1:
		dlssMode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssMode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssMode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssMode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssMode = sl::DLSSMode::eMaxQuality;
		break;
	}

	sl::DLSSOptions options{};
	options.mode = dlssMode;
	options.outputWidth = a_outputWidth;
	options.outputHeight = a_outputHeight;
	// The Vulkan scene chain is always FP16 HDR.
	options.colorBuffersHDR = sl::Boolean::eTrue;
	options.useAutoExposure = sl::Boolean::eTrue;
	options.preExposure = globals::state->preExposure;

	// Use the recommended preset for the detected NVIDIA architecture.
	if (isRTXBelow40Series) {
		options.dlaaPreset = sl::DLSSPreset::ePresetJ;
		options.ultraQualityPreset = sl::DLSSPreset::ePresetJ;
		options.qualityPreset = sl::DLSSPreset::ePresetJ;
		options.balancedPreset = sl::DLSSPreset::ePresetJ;
		options.performancePreset = sl::DLSSPreset::ePresetJ;
		options.ultraPerformancePreset = sl::DLSSPreset::ePresetM;
	} else if (isNvidiaGPU) {
		options.dlaaPreset = sl::DLSSPreset::ePresetJ;
		options.ultraQualityPreset = sl::DLSSPreset::ePresetJ;
		options.qualityPreset = sl::DLSSPreset::ePresetM;
		options.balancedPreset = sl::DLSSPreset::ePresetM;
		options.performancePreset = sl::DLSSPreset::ePresetM;
		options.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
	}

	static bool s_loggedPreset = false;
	if (!s_loggedPreset) {
		s_loggedPreset = true;
		logger::info("[Streamline] DLSS presets set (mode {}): quality={} (RTX40+={} below40={})",
			static_cast<int>(dlssMode), static_cast<int>(options.qualityPreset),
			isNvidiaGPU && !isRTXBelow40Series, isRTXBelow40Series);
	}

	// Set where the evaluate records, with this frame's values (the pre-exposure read above, the caller's mode and output size).
	const sl::ViewportHandle viewport = g_sl.viewport;
	const cs_SetOptions setOptions = [viewport, options] { return g_sl.slDLSSSetOptions(viewport, options); };

	const sl::Result evalRes = cs_EvaluateFeatureCore(sl::kFeatureDLSS, g_sl.viewport,
		a_colorIn, a_colorOut, a_depth, a_motionVectors,
		a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY,
		nullptr, &outputReady, &evaluationSkipped, setOptions);
	result = cs_ClassifyEvaluation(evalRes, outputReady, evaluationSkipped);

	static sl::Result s_loggedRes = sl::Result::eErrorNotInitialized;
	static uint32_t s_loggedDims = 0;
	const uint32_t dims = (a_renderWidth << 16) | (a_outputWidth & 0xFFFF);
	if (evalRes != s_loggedRes || dims != s_loggedDims) {
		s_loggedRes = evalRes;
		s_loggedDims = dims;
		logger::info("[Streamline] DLSS evaluate result={} render={}x{} output={}x{}",
			static_cast<int>(evalRes), a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight);
	}
	return result;
}

Streamline::EvaluationResult Streamline::EvaluateXeSS(ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut,
	ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_outputWidth, uint32_t a_outputHeight,
	uint32_t a_qualityMode, float a_sharpness,
	float a_jitterX, float a_jitterY)
{
	bool outputReady = false;
	bool evaluationSkipped = false;
	EvaluationResult result = EvaluationResult::kFailed;
	if (!initialized || !featureXeSS)
		return result;
	if (!a_colorIn || !a_colorOut || !a_depth || !a_motionVectors)
		return result;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!native && !dxvk->CommandResourcesReady())
		return result;

	sl::XeSSMode xessMode = sl::XeSSMode::eQuality;
	switch (a_qualityMode) {
	case 0:
		xessMode = sl::XeSSMode::eNativeAA;
		break;
	case 1:
		xessMode = sl::XeSSMode::eQuality;
		break;
	case 2:
		xessMode = sl::XeSSMode::eBalanced;
		break;
	case 3:
		xessMode = sl::XeSSMode::ePerformance;
		break;
	case 4:
		xessMode = sl::XeSSMode::eUltraPerformance;
		break;
	}

	sl::XeSSOptions xessOpts{};
	xessOpts.mode = xessMode;
	xessOpts.outputWidth = a_outputWidth;
	xessOpts.outputHeight = a_outputHeight;
	xessOpts.sharpness = a_sharpness;
	xessOpts.colorBuffersHDR = sl::Boolean::eTrue;
	// The input color is pre-exposed. The plugin gives XeSS its inverse as the exposure multiplier, as Intel's guide asks.
	xessOpts.preExposure = globals::state->preExposure;
	// Set where the evaluate records, with this frame's values, as for DLSS.
	const sl::ViewportHandle viewport = g_sl.viewport;
	const cs_SetOptions setOptions = [viewport, xessOpts] { return g_sl.slXeSSSetOptions(viewport, xessOpts); };

	const sl::Result evalRes = cs_EvaluateFeatureCore(sl::kFeatureXeSS, g_sl.viewport,
		a_colorIn, a_colorOut, a_depth, a_motionVectors,
		a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY,
		nullptr, &outputReady, &evaluationSkipped, setOptions);
	result = cs_ClassifyEvaluation(evalRes, outputReady, evaluationSkipped);

	static sl::Result s_loggedRes = sl::Result::eErrorNotInitialized;
	static uint32_t s_loggedDims = 0;
	const uint32_t dims = (a_renderWidth << 16) | (a_outputWidth & 0xFFFF);
	if (evalRes != s_loggedRes || dims != s_loggedDims) {
		s_loggedRes = evalRes;
		s_loggedDims = dims;
		logger::info("[Streamline] XeSS evaluate result={} render={}x{} output={}x{}",
			static_cast<int>(evalRes), a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight);
	}
	return result;
}

Streamline::EvaluationResult Streamline::EvaluateFSR(ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut,
	ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_outputWidth, uint32_t a_outputHeight,
	uint32_t a_qualityMode, float a_sharpness,
	float a_jitterX, float a_jitterY)
{
	bool outputReady = false;
	bool evaluationSkipped = false;
	EvaluationResult result = EvaluationResult::kFailed;
	if (!initialized || !featureFSR)
		return result;
	if (!a_colorIn || !a_colorOut || !a_depth || !a_motionVectors)
		return result;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!native && !dxvk->CommandResourcesReady())
		return result;

	sl::FSRMode fsrMode = sl::FSRMode::eMaxQuality;
	switch (a_qualityMode) {
	case 0:
		fsrMode = sl::FSRMode::eNativeAA;
		break;
	case 1:
		fsrMode = sl::FSRMode::eMaxQuality;
		break;
	case 2:
		fsrMode = sl::FSRMode::eBalanced;
		break;
	case 3:
		fsrMode = sl::FSRMode::eMaxPerformance;
		break;
	case 4:
		fsrMode = sl::FSRMode::eUltraPerformance;
		break;
	}

	sl::FSROptions fsrOpts{};
	fsrOpts.mode = fsrMode;
	fsrOpts.outputWidth = a_outputWidth;
	fsrOpts.outputHeight = a_outputHeight;
	fsrOpts.sharpness = a_sharpness;
	fsrOpts.colorBuffersHDR = sl::Boolean::eTrue;
	fsrOpts.preExposure = globals::state->preExposure;
	// Set where the evaluate records, with this frame's values, as for DLSS.
	const sl::ViewportHandle viewport = g_sl.viewport;
	const cs_SetOptions setOptions = [viewport, fsrOpts] { return g_sl.slFSRSetOptions(viewport, fsrOpts); };

	const sl::Result evalRes = cs_EvaluateFeatureCore(sl::kFeatureFSR, g_sl.viewport,
		a_colorIn, a_colorOut, a_depth, a_motionVectors,
		a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY,
		nullptr, &outputReady, &evaluationSkipped, setOptions);
	result = cs_ClassifyEvaluation(evalRes, outputReady, evaluationSkipped);

	static sl::Result s_loggedRes = sl::Result::eErrorNotInitialized;
	static uint32_t s_loggedDims = 0;
	const uint32_t dims = (a_renderWidth << 16) | (a_outputWidth & 0xFFFF);
	if (evalRes != s_loggedRes || dims != s_loggedDims) {
		s_loggedRes = evalRes;
		s_loggedDims = dims;
		logger::info("[Streamline] FSR evaluate result={} render={}x{} output={}x{}",
			static_cast<int>(evalRes), a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight);
	}
	return result;
}

bool Streamline::EvaluateFSRFrameGen(ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_hudlessColor,
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_outputWidth, uint32_t a_outputHeight,
	float a_jitterX, float a_jitterY, bool a_depthCopy)
{
	// Isolate FSR frame-generation preparation from viewport 0 upscaling tags and constants.
	// Not separated from render-graph epochs (Upscaling::SeparateFromGraphEpochs): if a graph epoch is the last GPU work before this
	// evaluate, its output can be corrupt (docs/development/render-graph.md, "Hazard: external Vulkan work directly after a graph epoch").
	if (!initialized || !featureFSRFG)
		return false;
	if (!a_depth || !a_motionVectors || !a_hudlessColor)
		return false;

	// Graph modes: the UI stage's pass after the hudless copy. Plain D3D11 mode keeps the interop ring below.
	if (ORGUpscaling::Get().RecordsFrameGenInputs()) {
		bool queued = false;
		const sl::Result res = cs_RecordFrameGenInputs(a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY, a_depthCopy, &queued);
		if (queued)
			g_sl.fsrfgPreparedThisFrame.store(true, std::memory_order_release);
		static sl::Result s_loggedPassRes = sl::Result::eErrorNotInitialized;
		if (res != s_loggedPassRes) {
			s_loggedPassRes = res;
			logger::info("[Streamline] FSR FG-prepare (UI stage pass) result={} render={}x{}", static_cast<int>(res), a_renderWidth, a_renderHeight);
		}
		return res == sl::Result::eOk && queued;
	}

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady())
		return false;

	bool evaluationSubmitted = false;
	bool accepted = false;
	// FSR-FG interpolates on its own viewport, separate from the viewport that carries the
	// swapchain-level options. Evaluating on the options viewport instead was measured worse:
	// present rate ran away to 85 fps against a 10 fps render and the image got darker still.
	const sl::ViewportHandle fgViewport{ 1 };
	const sl::Result evalRes = cs_EvaluateFeatureCore(sl::kFeatureFSR_G, fgViewport,
		nullptr, nullptr, a_depth, a_motionVectors,
		a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY,
		a_hudlessColor, &evaluationSubmitted);
	accepted = evalRes == sl::Result::eOk && evaluationSubmitted;

	if (accepted)
		g_sl.fsrfgPreparedThisFrame.store(true, std::memory_order_release);

	static sl::Result s_loggedRes = sl::Result::eErrorNotInitialized;
	if (evalRes != s_loggedRes) {
		s_loggedRes = evalRes;
		logger::info("[Streamline] FSR FG-prepare result={} render={}x{}", static_cast<int>(evalRes), a_renderWidth, a_renderHeight);
	}
	return accepted;
}

bool Streamline::EnsureFSRFGPresentState()
{
	// The FSR-FG counterpart to EnsureDLSSGPresentTag.
	//
	// BeginRenderFrame discards any stale prepared frame, but it is driven from
	// Main_UpdateJitter, a 3D render-path hook that does not run on the main menu or while the
	// game is otherwise not rendering a scene. EvaluateFSRFrameGen does not run there either --
	// there is no hudless colour, depth or motion vector to hand FFX. So on those frames nothing
	// discards and nothing prepares, and FFX happily keeps interpolating whatever it last held:
	// measured on the main menu as a black screen presenting at 113 fps against a 10 fps render,
	// which also reads as severe input lag.
	//
	// Discard whatever FFX still holds so it does not keep re-presenting a stale interpolation.
	//
	// Two alternatives were measured and are worse: gating sl.fsr_g's `enabled` option off drove
	// the FFX swapchain to 642 fps against a 10 fps render, and doing nothing at all left it at
	// 113 fps. This keeps it at a clean 2x.
	//
	// Note none of these fix the HDR darkness -- that is independent of interpolation entirely
	// (it reproduces with interpolation disabled). See cs-fsrfg-hdr-black in memory.
	if (!initialized || !featureFSRFG ||
		!g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire))
		return false;

	const bool prepared = g_sl.fsrfgPreparedThisFrame.load(std::memory_order_acquire);

	{
		static bool s_lastPrepared = true;
		if (s_lastPrepared != prepared) {
			s_lastPrepared = prepared;
			if (prepared)
				logger::info("[Streamline] FSR-FG interpolation inputs restored");
			else
				logger::warn("[Streamline] FSR-FG has no interpolation inputs - discarding prepared frames");
		}
	}

	// Native mode: no discard from the render thread (BeginRenderFrame).
	if (!prepared && !native)
		(void)DiscardFSRFrameGenerationPreparedFrame();

	return prepared;
}

bool Streamline::SetDLSSGMode(bool a_enable, uint32_t a_displayWidth, uint32_t a_displayHeight,
	uint32_t a_numFramesToGenerate, bool a_autoMode, bool a_dynamic, float a_dynamicTargetFps)
{
	if (!initialized || !featureDLSSG)
		return false;

	// Do not call the options entry point while DLSS-G is runtime-unloaded.
	if (!g_dlssgCurrentlyLoaded.load(std::memory_order_acquire))
		return false;

	// Clamp the requested multiplier to the reported hardware limit.
	const uint32_t maxFrames = g_sl.dlssgMaxFramesToGenerate.load(std::memory_order_acquire);
	uint32_t numFrames = a_numFramesToGenerate < 1u ? 1u : a_numFramesToGenerate;
	if (maxFrames > 0u && numFrames > maxFrames)
		numFrames = maxFrames;

	// Reissue options each frame; cached values only suppress duplicate logging.
	const bool changed = !(g_sl.dlssgModeCached && g_sl.dlssgModeOn == a_enable &&
		g_sl.dlssgCachedNumFrames == numFrames && g_sl.dlssgCachedAuto == a_autoMode &&
		g_sl.dlssgCachedDynamic == a_dynamic && g_sl.dlssgCachedDynamicFps == a_dynamicTargetFps &&
		g_sl.dlssgCachedDisplayW == a_displayWidth && g_sl.dlssgCachedDisplayH == a_displayHeight);
	const bool wasModeOn = g_sl.dlssgModeOn;

	bool succeeded = false;
	sl::DLSSGOptions options{};
	options.mode = !a_enable ? sl::DLSSGMode::eOff :
	               a_dynamic ? sl::DLSSGMode::eDynamic :
	               a_autoMode ? sl::DLSSGMode::eAuto :
	                            sl::DLSSGMode::eOn;
	options.numFramesToGenerate = numFrames;
	if (a_dynamic)
		options.dynamicTargetFrameRate = a_dynamicTargetFps;
	// Retain resources across temporary loading-screen and menu disables.
	options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
	options.mvecDepthWidth = a_displayWidth;
	options.mvecDepthHeight = a_displayHeight;
	options.colorWidth = a_displayWidth;
	options.colorHeight = a_displayHeight;
	// Volatile inputs are copied into Streamline-owned resources before present.
	//
	// eBlockNoClientQueues is the faster-sounding option and was what this used, but Streamline
	// only permits it if the client waits on DLSSGState::inputsProcessingCompletionFence before
	// modifying or destroying any resource tagged as a DLSS-G input. CS never reads that fence,
	// so it can overwrite depth, motion vectors or the hudless colour while DLSS-G is still
	// consuming them for the previously presented frame. The symptom of that race is an
	// occasional corrupt generated frame rather than a measurable slowdown.
	//
	// The queue parallelism it buys does not show up here in any case, and the two things it
	// might plausibly have bought were checked separately. It does not raise throughput: with
	// eBlockNoClientQueues the rendered rate stayed at exactly 30.0 fps against a 60 Hz mode,
	// identical to the default, because what pins the rendered rate is sl.dlss_g pacing presents
	// to the display refresh -- not the client queue being blocked. (Forcing a tearing present
	// mode and forcing eDynamic with a 240 fps target were also both measured and changed
	// nothing; the latter simply stopped generating.) Nor does it smooth delivery: six paired
	// runs at the refresh rate, a fresh game instance each, were indistinguishable -- frame-time
	// deviation 0.012 / 0.014 / 0.012 ms against 0.013 / 0.014 / 0.019 ms, with GPU-busy differences
	// swamped by which way the camera happened to be facing. The mode was trading a correctness
	// guarantee for nothing measurable, so take the documented default, whose contract CS meets.
	options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
	const sl::Result res = g_sl.slDLSSGSetOptions(g_sl.viewport, options);
	if (res != sl::Result::eOk) {
		if (changed)
			logger::warn("[Streamline] slDLSSGSetOptions failed (result {})", static_cast<int>(res));
	} else {
		succeeded = true;
		g_sl.dlssgModeCached = true;
		g_sl.dlssgModeOn = a_enable;
		g_sl.dlssgCachedNumFrames = numFrames;
		g_sl.dlssgCachedAuto = a_autoMode;
		g_sl.dlssgCachedDynamic = a_dynamic;
		g_sl.dlssgCachedDynamicFps = a_dynamicTargetFps;
		g_sl.dlssgCachedDisplayW = a_displayWidth;
		g_sl.dlssgCachedDisplayH = a_displayHeight;
		if (!a_enable || !wasModeOn)
			g_sl.dlssgCloneTagsPrimed.store(false, std::memory_order_release);
		if (changed)
			logger::info("[Streamline] DLSS-G mode={} ({}) numFrames={} targetFps={} (max {}) display={}x{}", a_enable,
				!a_enable ? "off" : a_dynamic ? "dynamic" : a_autoMode ? "auto" : "on", numFrames, a_dynamicTargetFps, maxFrames,
				a_displayWidth, a_displayHeight);
	}
	return succeeded;
}

bool Streamline::SetFSRFrameGen(bool a_enable, bool a_hdr,
	bool a_debugView, bool a_debugTearLines, bool a_debugPacingLines, bool a_onlyPresentGenerated)
{
	// The caller retries until the runtime-loaded plugin accepts the option.
	if (!initialized || !featureFSRFG || !g_sl.slFSRFrameGenerationSetOptions)
		return false;
	if (!g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire))
		return false;

	bool ok = false;
	sl::FSRFrameGenOptions options{};
	options.enabled = a_enable ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	options.colorBuffersHDR = a_hdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	options.debugView = a_debugView ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	options.debugTearLines = a_debugTearLines ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	options.debugPacingLines = a_debugPacingLines ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	options.onlyPresentGenerated = a_onlyPresentGenerated ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	// FFX places its interpolated frame at (average frametime - variance * varianceFactor -
	// safetyMarginInMs), not at the midpoint. Cutting that subtraction roughly halves the visible
	// hitch rate when a frame cap is in force -- display intervals more than 10 ms off the mean,
	// over the settled half of ten captures of the capped bench:
	//
	//     FFX defaults 0.1/0.1   0.84% of frames        this   0.20%
	//
	// See tools/pacing/README.md for the uncapped measurements and why this pair is used there too.
	options.pacingSafetyMarginMs = 0.01f;
	options.pacingVarianceFactor = 0.0f;
	// Native mode replaces the swap chain itself (NativePresenter). Guest leaves the recreate trigger to the plugin.
	options.hostRecreatesSwapchain = native ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	const sl::Result res = g_sl.slFSRFrameGenerationSetOptions(g_sl.viewport, options);
	if (res != sl::Result::eOk) {
		logger::error("[Streamline] slFSRFrameGenerationSetOptions failed (result {})", static_cast<int>(res));
	} else {
		ok = true;
		g_fsrfgOwnsPresent.store(a_enable, std::memory_order_release);
		if (!a_enable)
			g_sl.frameGenerationMultiplier.store(1, std::memory_order_release);
		logger::info("[Streamline] FSR frame generation {} (colorBuffersHDR={})",
			a_enable ? "enabled" : "disabled", a_hdr);
	}
	return ok;
}

void Streamline::CaptureFSRFrameGenState()
{
	if (!initialized || !featureFSRFG || !g_sl.slFSRGetFrameGenState)
		return;
	sl::FSRFrameGenState state{};
	const sl::Result res = g_sl.slFSRGetFrameGenState(g_sl.viewport, state);
	if (res == sl::Result::eOk) {
		g_sl.frameGenerationMultiplier.store(
			std::max(state.numFramesActuallyPresented, 1u), std::memory_order_release);
		g_sl.fsrTotalPresentedFrames.store(state.totalPresentedFrames, std::memory_order_release);
		// Native mode records each frame's prepare and its present on the submission thread, in order. A present that found a
		// frame prepared for another one means that order broke.
		if (native && state.foreignPreparedFrames != 0)
			stl::report_and_fail(fmt::format("[Streamline] FSR-FG presented {} frame(s) with another frame's prepared inputs; the native submission order broke",
				state.foreignPreparedFrames));
		// FSR-FG had no equivalent of the DLSS-G present-state log, so "frame generation is on
		// but generating nothing" was invisible on this path and the overlay's Post-FG FPS was
		// the only clue. Sample on the same cadence DLSS-G uses.
		// Report every CHANGE in the presented count. A periodic sample cannot answer
		// "is frame generation dropping out while it is switched on", because it cannot
		// distinguish a steady 1 from a 2 that briefly fell to 1 between samples.
		static uint32_t s_fsrLastPresented = UINT32_MAX;
		if (s_fsrLastPresented != state.numFramesActuallyPresented) {
			const uint32_t prev = s_fsrLastPresented;
			s_fsrLastPresented = state.numFramesActuallyPresented;
			if (prev != UINT32_MAX && state.numFramesActuallyPresented < 2u)
				logger::warn("[Streamline] FSR-FG stopped generating: presenting {} (was {}), total presented {}",
					state.numFramesActuallyPresented, prev, state.totalPresentedFrames);
			else
				logger::info("[Streamline] FSR-FG presenting {} frame(s) (was {}), total presented {}",
					state.numFramesActuallyPresented, prev, state.totalPresentedFrames);
		}
	} else {
		static sl::Result s_lastFsrStateRes = sl::Result::eOk;
		if (res != s_lastFsrStateRes) {
			s_lastFsrStateRes = res;
			logger::warn("[Streamline] slFSRGetFrameGenState failed (result {}) - Post-FG FPS falls back to an estimate",
				static_cast<int>(res));
		}
	}
}

uint64_t Streamline::GetTotalPresentedFrames() const
{
	return g_sl.fsrTotalPresentedFrames.load(std::memory_order_acquire);
}

void Streamline::QueryDLSSGCapabilities()
{
	// Streamline requires this state query on the present thread.
	if (!initialized || !featureDLSSG ||
		!g_dlssgCurrentlyLoaded.load(std::memory_order_acquire))
		return;
	if (g_sl.dlssgMaxFramesToGenerate.load(std::memory_order_acquire) != 0u)
		return;
	sl::DLSSGState state{};
	if (g_sl.slDLSSGGetState(g_sl.viewport, state, nullptr) == sl::Result::eOk && state.numFramesToGenerateMax > 0u) {
		g_sl.dlssgMaxFramesToGenerate.store(state.numFramesToGenerateMax, std::memory_order_release);
		g_sl.dlssgDynamicSupported.store(state.bIsDynamicMFGSupported == sl::Boolean::eTrue, std::memory_order_release);
		logger::info("[Streamline] DLSS-G numFramesToGenerateMax = {} (max {}x multiplier), DynamicMFG supported = {}",
			state.numFramesToGenerateMax, state.numFramesToGenerateMax + 1u, state.bIsDynamicMFGSupported == sl::Boolean::eTrue);
	}
}

uint32_t Streamline::GetDLSSGMaxFramesToGenerate() const
{
	return g_sl.dlssgMaxFramesToGenerate.load(std::memory_order_acquire);
}

uint32_t Streamline::GetFrameGenerationMultiplier() const
{
	return g_sl.frameGenerationMultiplier.load(std::memory_order_acquire);
}

bool Streamline::IsDLSSGVsyncSupported() const
{
	return g_sl.dlssgVsyncSupported.load(std::memory_order_acquire);
}

bool Streamline::IsDLSSGDynamicSupported() const
{
	return g_sl.dlssgDynamicSupported.load(std::memory_order_acquire);
}

// Names the precondition that pushed DLSS-G onto the passthrough path. Every early return in
// TagDLSSGResources used to be silent, which made "frame generation is on but nothing is
// generated" -- which still costs the latency of the frame-generation present path -- look
// identical to a dozen unrelated causes. Logs only when the reason changes, so it cannot spam.
static void cs_NoteDlssgTagSkip(const char* a_reason)
{
	static const char* s_last = nullptr;
	if (s_last == a_reason)
		return;
	const char* const prev = s_last;
	s_last = a_reason;
	if (a_reason)
		logger::warn("[Streamline] DLSS-G tagging real inputs SKIPPED ({}) - passthrough present, no frames generated", a_reason);
	else if (prev)
		logger::info("[Streamline] DLSS-G tagging real inputs again");
}

void Streamline::TagDLSSGResources(
ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_hudlessColor, uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_displayWidth, uint32_t a_displayHeight, float a_jitterX, float a_jitterY, bool a_depthCopy)
{
	// DLSS-G interpolates from these tags at present, through Streamline's present hook. It is not separated from render-graph epochs:
	// if a graph epoch is the last GPU work before it, its output can be corrupt (docs/development/render-graph.md, "Hazard: external
	// Vulkan work directly after a graph epoch").
	if (!initialized || !featureDLSSG) {
		cs_NoteDlssgTagSkip("DLSS-G unavailable");
		return;
	}
	if (!a_depth || !a_motionVectors) {
		cs_NoteDlssgTagSkip("game gave no depth or motion-vector target");
		return;
	}

	// Graph modes: the UI stage's pass after the hudless copy. Plain D3D11 mode keeps the interop ring below.
	if (ORGUpscaling::Get().RecordsFrameGenInputs() || native) {
		// Every gameplay frame has its constants, tagged or not.
		const bool constants = cs_EnsureDLSSGConstants(a_displayWidth, a_displayHeight, a_jitterX, a_jitterY);
		if (!a_hudlessColor || !ORGUpscaling::Get().RecordsFrameGenInputs()) {
			cs_NoteDlssgTagSkip("the graph holds no hudless colour");
			ClearDLSSGTags();
			return;
		}
		if (!g_sl.dlssgCloneTagsPrimed.load(std::memory_order_acquire)) {
			cs_NoteDlssgTagSkip("clone tags not primed yet");
			ClearDLSSGTags();
			return;
		}
		if (!constants) {
			cs_NoteDlssgTagSkip("the frame has no constants");
			ClearDLSSGTags();
			return;
		}
		bool queued = false;
		(void)cs_RecordDLSSGInputs(a_renderWidth, a_renderHeight, a_displayWidth, a_displayHeight, a_depthCopy, &queued);
		if (queued) {
			g_sl.dlssgTaggedThisFrame = true;
			cs_NoteDlssgTagSkip(nullptr);
		} else {
			cs_NoteDlssgTagSkip("the inputs pass is not scheduled yet");
			ClearDLSSGTags();
		}
		return;
	}

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady()) {
		cs_NoteDlssgTagSkip("interop command resources not ready");
		return;
	}
	if (!g_sl.dlssgCloneTagsPrimed.load(std::memory_order_acquire)) {
		cs_NoteDlssgTagSkip("clone tags not primed yet");
		ClearDLSSGTags();
		return;
	}

	sl::FrameToken* token = RenderFrameToken();
	if (!token) {
		cs_NoteDlssgTagSkip("no render frame token");
		return;
	}

	VkDevice vkDevice = dxvk->GetDevice();
	const cs_VulkanProcAttempt createProcAttempt = cs_GetDeviceProcAddrSEH(
		dxvk->GetDeviceProcAddr(), vkDevice, "vkCreateImageView");
	const cs_VulkanProcAttempt destroyProcAttempt = cs_GetDeviceProcAddrSEH(
		dxvk->GetDeviceProcAddr(), vkDevice, "vkDestroyImageView");
	if (createProcAttempt.exceptionCode || destroyProcAttempt.exceptionCode) {
		return;
	}
	auto vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(createProcAttempt.function);
	auto vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(destroyProcAttempt.function);
	if (!vkCreateImageView || !vkDestroyImageView)
		return;
	ID3D11Resource* resources[] = { a_depth, a_motionVectors, a_hudlessColor };
	VkImageView views[3]{};
	uint32_t viewCount = 0;
	bool viewCreationTerminalFault = false;
	const auto destroyViews = [&]() {
		return cs_DestroyViews(dxvk, vkDevice, vkDestroyImageView, views, viewCount,
			resources, static_cast<uint32_t>(std::size(resources)));
	};
	const auto abandonViewsAfterCreationFailure = [&]() {
		if (!viewCreationTerminalFault) {
			destroyViews();
			return;
		}
		// The destroy entry point already faulted on these handles; calling it again would fault
		// again, so abandon the views rather than retrying the destroy.
		std::fill(std::begin(views), std::end(views), VK_NULL_HANDLE);
	};

	const auto makeResource = [&](ID3D11Resource* a_res, sl::Resource& a_out,
		                          sl::SubresourceRange& a_subresource) {
		if (viewCount >= std::size(views))
			return false;
		VkImageView& view = views[viewCount];
		if (!cs_WrapInteropImage(dxvk, vkDevice, vkCreateImageView, a_res, a_out, a_subresource, view, viewCreationTerminalFault))
			return false;
		++viewCount;
		return true;
	};

	sl::Resource depthRes{}, mvecRes{};
	sl::SubresourceRange depthRange{}, mvecRange{}, hudlessRange{};
	if (!makeResource(a_depth, depthRes, depthRange) ||
		!makeResource(a_motionVectors, mvecRes, mvecRange)) {
		cs_NoteDlssgTagSkip("could not wrap depth/motion-vector resources for Vulkan");
		abandonViewsAfterCreationFailure();
		return;
	}

	sl::Extent extent{};
	extent.width = a_renderWidth;
	extent.height = a_renderHeight;

	sl::ResourceTag tags[3];
	uint32_t tagCount = 0;
	tags[tagCount++] = { &depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent };
	tags[tagCount++] = { &mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent };

	// HUD-less color uses display dimensions rather than the render subrect.
	sl::Extent displayExtent{};
	displayExtent.width = a_displayWidth;
	displayExtent.height = a_displayHeight;
	sl::Resource hudlessRes{};
	const uint32_t viewsBeforeHudless = viewCount;
	if (a_hudlessColor) {
		if (makeResource(a_hudlessColor, hudlessRes, hudlessRange)) {
			tags[tagCount++] = { &hudlessRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &displayExtent };
		} else if (viewCount != viewsBeforeHudless) {
			abandonViewsAfterCreationFailure();
			return;
		} else {
			tags[tagCount++] = { nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, nullptr };
		}
	} else {
		// Clear stale HUD-less input when capture is unavailable.
		tags[tagCount++] = { nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, nullptr };
	}

	sl::Result tagResult = sl::Result::eErrorNotInitialized;
	bool lifetimesRetained = false;
	if (cs_SubmitPresentTags(dxvk, *token, g_sl.viewport, tags, tagCount,
			views, viewCount, resources, static_cast<uint32_t>(std::size(resources)), tagResult,
			lifetimesRetained)) {
		g_sl.dlssgTaggedThisFrame = true;
		cs_NoteDlssgTagSkip(nullptr);
	} else {
		cs_NoteDlssgTagSkip("tag submission rejected");
		if (!lifetimesRetained)
			destroyViews();
		logger::error("[Streamline] DLSS-G present tagging failed (last tag result {})",
			static_cast<int>(tagResult));
	}
}

void Streamline::ClearDLSSGTags()
{
	if (!initialized || !featureDLSSG)
		return;

	sl::FrameToken* token = RenderFrameToken();
	if (!token)
		return;

	// Null tags force passthrough when interpolation inputs are unavailable.
	sl::ResourceTag tags[] = {
		sl::ResourceTag{ nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, nullptr },
		sl::ResourceTag{ nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, nullptr },
		sl::ResourceTag{ nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, nullptr },
	};
	// Native mode: null tags copy nothing, so they need no command list. The inputs pass did not run for this frame.
	if (native) {
		if (const sl::Result tagResult = g_sl.slSetTagForFrame(*token, g_sl.viewport, tags, static_cast<uint32_t>(std::size(tags)), nullptr); tagResult == sl::Result::eOk)
			g_sl.dlssgTaggedThisFrame = true;
		else
			logger::error("[Streamline] DLSS-G passthrough tags failed (result {})", static_cast<int>(tagResult));
		return;
	}
	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady())
		return;
	sl::Result tagResult = sl::Result::eErrorNotInitialized;
	bool lifetimesRetained = false;
	if (cs_SubmitPresentTags(dxvk, *token, g_sl.viewport, tags,
			static_cast<uint32_t>(std::size(tags)), nullptr, 0, nullptr, 0, tagResult,
			lifetimesRetained)) {
		g_sl.dlssgTaggedThisFrame = true;
	} else {
		logger::error("[Streamline] DLSS-G passthrough tag submission failed (result {})",
			static_cast<int>(tagResult));
	}
}

bool Streamline::EnsureDLSSGPresentTag()
{
	// Supply passthrough tags when the render pass did not provide interpolation inputs.
	if (!initialized || !featureDLSSG ||
		!g_dlssgCurrentlyLoaded.load(std::memory_order_acquire))
		return false;
	// Track the passthrough fallback separately from the reasons above: this also catches the
	// cases where TagDLSSGResources was never called at all (paused, a menu is open, or the
	// presenter was not ready), which otherwise leave no trace and read in the log exactly like
	// healthy frame generation while generating nothing.
	const bool hadRealTags = g_sl.dlssgTaggedThisFrame;
	if (!hadRealTags)
		ClearDLSSGTags();
	{
		static bool s_lastHadRealTags = true;
		if (s_lastHadRealTags != hadRealTags) {
			s_lastHadRealTags = hadRealTags;
			if (hadRealTags)
				logger::info("[Streamline] DLSS-G interpolation inputs restored");
			else
				logger::warn("[Streamline] DLSS-G falling back to passthrough tags - render pass submitted no interpolation inputs this frame");
		}
	}
	return g_sl.dlssgTaggedThisFrame;
}

bool Streamline::CompleteSwapchainTeardown()
{
	return SwapchainTornDown();
}

void Streamline::TurnDLSSGOffBeforeSwapchainChange()
{
	if (!initialized || !native || !featureDLSSG || !g_dlssgCurrentlyLoaded.load(std::memory_order_acquire) || !g_sl.dlssgModeOn)
		return;
	// Streamline asks for the mode off before any swap chain change. The presents already queued take it, then the change runs.
	if (!SetDLSSGMode(false, g_sl.dlssgCachedDisplayW, g_sl.dlssgCachedDisplayH))
		stl::report_and_fail("[Streamline] DLSS-G refused its mode off before a swap chain change");
	logger::info("[Streamline] DLSS-G mode off before a swap chain change");
}

void Streamline::SetDLSSGDesiredLoaded(bool a_loaded)
{
	g_dlssgDesiredLoaded.store(a_loaded, std::memory_order_release);
}

bool Streamline::IsDLSSGLoaded() const
{
	return g_dlssgCurrentlyLoaded.load(std::memory_order_acquire);
}

bool Streamline::IsDLSSGLoadSettled() const
{
	return g_dlssgDesiredLoaded.load(std::memory_order_acquire) ==
	       g_dlssgCurrentlyLoaded.load(std::memory_order_acquire);
}

void Streamline::SetFSRFGDesiredLoaded(bool a_loaded)
{
	g_fsrfgDesiredLoaded.store(a_loaded, std::memory_order_release);
}

bool Streamline::IsFSRFGLoaded() const
{
	return g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire);
}

bool Streamline::IsFSRFGLoadSettled() const
{
	return g_fsrfgDesiredLoaded.load(std::memory_order_acquire) ==
	       g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire);
}

bool Streamline::IsFSRFGPresentOwner() const
{
	return g_fsrfgOwnsPresent.load(std::memory_order_acquire);
}


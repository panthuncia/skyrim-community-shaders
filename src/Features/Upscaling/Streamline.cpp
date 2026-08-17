#include "Streamline.h"

#include "DXVKInterop.h"

#include "../../DxvkLoader.h"
#include "../../Globals.h"
#include "../../State.h"
#include "../../Utils/Game.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

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
		PFun_slIsFeatureLoaded* slIsFeatureLoaded = nullptr;

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

		uint32_t renderFrameId = 0;

		// Disable dispatch after an SEH fault to prevent repeated crashes.
		std::atomic<bool> dispatchFaulted{ false };

		bool reflexCacheValid = false;
		sl::ReflexMode reflexCachedMode = sl::ReflexMode::eOff;
		uint32_t reflexCachedFrameLimitUs = 0;

		bool dlssgModeCached = false;
		std::atomic<bool> dlssgModeOn{ false };
		uint32_t dlssgCachedRenderW = 0, dlssgCachedRenderH = 0;
		uint32_t dlssgCachedDisplayW = 0, dlssgCachedDisplayH = 0;
		uint32_t dlssgCachedNumFrames = 0;
		bool dlssgCachedAuto = false;
		bool dlssgCachedDynamic = false;
		float dlssgCachedDynamicFps = 0.0f;
		std::atomic<uint32_t> dlssgMaxFramesToGenerate = 0;
		std::atomic<bool> dlssgDynamicSupported = false;
		std::atomic<uint32_t> frameGenerationMultiplier = 1;
		sl::DLSSGOptions dlssgPendingOptions{};
		std::atomic<bool> dlssgOptionsPending{ false };
		bool dlssgPendingEnable = false;
		uint32_t dlssgPendingRenderW = 0, dlssgPendingRenderH = 0;
		uint32_t dlssgPendingDisplayW = 0, dlssgPendingDisplayH = 0;

		// Present requires either a valid or passthrough tag every frame.
		bool dlssgTaggedThisFrame = false;
		uint32_t viewport0ConstantsFrame = UINT32_MAX;
		std::atomic<bool> dlssgCloneTagsPrimed{ false };

		// slDLSSGGetState is not thread-safe. The state query runs on DXVK's
		// Vulkan present thread, while option changes originate on the game thread.
		std::mutex dlssgApiMutex;
		std::array<std::atomic<uint32_t>, 64> presentMarkerFrames{};
		std::atomic<uint32_t> presentMarkerHead{ 0 };
		std::atomic<uint32_t> presentMarkerTail{ 0 };
		std::atomic<uint32_t> activePresentMarkerFrame{ 0 };
		std::atomic<uint32_t> dlssgOptionsEpoch{ 0 };
		std::atomic<uint32_t> dlssgLedgerBudget{ 0 };
		std::atomic<uint64_t> activeDxvkFrameId{ 0 };
		std::atomic<uint64_t> activeSwapchainSerial{ 0 };
		std::atomic<uint64_t> activePresentStartNs{ 0 };
		std::atomic<uint64_t> renderHeartbeatNs{ 0 };
		std::atomic<uint64_t> presentHeartbeatNs{ 0 };
		std::atomic<bool> watchdogTriggered{ false };
		std::jthread watchdog;
	} g_sl;

	struct DxvkPresentCallbackInfo
	{
		uint32_t size;
		uint32_t version;
		uint32_t frameGenOwner;
		uint32_t imageIndex;
		uint64_t frameId;
		uint64_t swapchain;
		uint64_t swapchainSerial;
		uint64_t presenter;
		uint64_t queue;
		uint64_t presentWaitGeneration;
		uint32_t pendingPresentWaitCount;
		int32_t presentResult;
	};

	uint64_t PresentClockNs()
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	bool IsForegroundSkyrim()
	{
		HWND window = GetForegroundWindow();
		if (!window)
			return false;
		DWORD processId = 0;
		GetWindowThreadProcessId(window, &processId);
		return processId == GetCurrentProcessId();
	}

	void RequestForcedTdr()
	{
		wchar_t systemDir[MAX_PATH]{};
		if (!GetSystemDirectoryW(systemDir, MAX_PATH))
			return;
		std::wstring command = L"\"" + std::wstring(systemDir) +
			L"\\schtasks.exe\" /Run /TN \"CommunityShaders GPU Recovery\"";
		STARTUPINFOW startup{ sizeof(startup) };
		PROCESS_INFORMATION process{};
		if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
			logger::critical("[DLSSG-Watchdog] requested forced TDR through scheduled recovery task");
		} else {
			logger::critical("[DLSSG-Watchdog] recovery task launch failed (Win32 error {}); run community_shaders_recover_hung_gpu.ps1 manually",
				GetLastError());
		}
	}

	void StartDlssgWatchdog()
	{
		if (g_sl.watchdog.joinable())
			return;
		g_sl.watchdog = std::jthread([](std::stop_token stop) {
			SetThreadDescription(GetCurrentThread(), L"CS DLSS-G hang watchdog");
			while (!stop.stop_requested()) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				if (!g_sl.dlssgModeOn.load(std::memory_order_acquire)) {
					g_sl.watchdogTriggered.store(false, std::memory_order_release);
					continue;
				}
				const uint64_t now = PresentClockNs();
				const uint64_t render = g_sl.renderHeartbeatNs.load(std::memory_order_acquire);
				const uint64_t present = g_sl.presentHeartbeatNs.load(std::memory_order_acquire);
				constexpr uint64_t timeoutNs = 8'000'000'000ull;
				if (!render || !present || now - render < timeoutNs || now - present < timeoutNs ||
					!IsForegroundSkyrim())
					continue;
				bool expected = false;
				if (g_sl.watchdogTriggered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
					logger::critical("[DLSSG-Watchdog] render and present stalled for {} ms; forcing WDDM recovery",
						(now - std::max(render, present)) / 1'000'000ull);
					RequestForcedTdr();
				}
			}
		});
	}

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
		if (!g_sl.slSetFeatureLoaded || g_sl.dispatchFaulted.load(std::memory_order_acquire))
			return false;

		// The present thread calls into DLSS-G through SetOptions/GetState. Serialize
		// feature load changes with those calls so Streamline cannot tear down its
		// Vulkan dispatch state while one of them is still submitting work. Publish
		// the unloaded state before calling Streamline so newly arriving callbacks
		// bail out instead of waiting to use stale entry points.
		if (a_feature == sl::kFeatureDLSS_G) {
			g_sl.dlssgApiMutex.lock();
			const bool lockedWant = a_desired.load(std::memory_order_acquire);
			const bool wasLoaded = a_current.load(std::memory_order_acquire);
			if (lockedWant == wasLoaded) {
				g_sl.dlssgApiMutex.unlock();
				return true;
			}

			if (!lockedWant)
				a_current.store(false, std::memory_order_release);

			bool succeeded = false;
			__try {
				succeeded = g_sl.slSetFeatureLoaded(a_feature, lockedWant) == sl::Result::eOk;
				if (succeeded && lockedWant) {
					g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void*&>(g_sl.slDLSSGSetOptions));
					g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", reinterpret_cast<void*&>(g_sl.slDLSSGGetState));
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				g_sl.dispatchFaulted = true;
			}

			if (succeeded) {
				a_current.store(lockedWant, std::memory_order_release);
			} else {
				a_current.store(wasLoaded, std::memory_order_release);
			}
			g_sl.dlssgApiMutex.unlock();
			return succeeded;
		}

		__try {
			if (g_sl.slSetFeatureLoaded(a_feature, want) != sl::Result::eOk)
				return false;
			a_current.store(want, std::memory_order_release);
			if (a_feature == sl::kFeatureFSR_G && !want)
				g_fsrfgOwnsPresent.store(false, std::memory_order_release);
			if (want) {
				if (a_feature == sl::kFeatureDLSS_G) {
					g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void*&>(g_sl.slDLSSGSetOptions));
					g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", reinterpret_cast<void*&>(g_sl.slDLSSGGetState));
				} else if (a_feature == sl::kFeatureFSR_G) {
					g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationSetOptions", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationSetOptions));
					g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRGetFrameGenState", reinterpret_cast<void*&>(g_sl.slFSRGetFrameGenState));
					g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationDiscardPreparedFrame", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationDiscardPreparedFrame));
					g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationOwnsSwapchain", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationOwnsSwapchain));
					g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationCompleteSwapchainTeardown", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationCompleteSwapchainTeardown));
				}
			}
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			g_sl.dispatchFaulted = true;
			return false;
		}
	}

	// Runs between DXVK swapchain destruction and creation.
	bool DxvkSwapchainTornDownCallback()
	{
		if (g_sl.dispatchFaulted.load(std::memory_order_acquire))
			return false;
		const bool wantDLSSG = g_dlssgDesiredLoaded.load(std::memory_order_acquire);
		const bool wantFSRFG = g_fsrfgDesiredLoaded.load(std::memory_order_acquire);
		if (wantDLSSG && wantFSRFG) {
			logger::error("[Streamline] refusing to load both frame-generation present plugins");
			return false;
		}

		// Per-swapchain options and semaphores are invalid after teardown.
		g_sl.dlssgModeCached = false;
		g_sl.dlssgModeOn = false;
		g_sl.dlssgOptionsPending = false;
		g_sl.dlssgCloneTagsPrimed.store(false, std::memory_order_release);

		if (g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire)) {
			if (!g_sl.slFSRFrameGenerationCompleteSwapchainTeardown)
				return false;
			bool teardownComplete = false;
			const bool releaseFeatureContext = !g_fsrfgDesiredLoaded.load(std::memory_order_acquire);
			__try {
				teardownComplete = g_sl.slFSRFrameGenerationCompleteSwapchainTeardown(releaseFeatureContext);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				g_sl.dispatchFaulted = true;
			}
			if (!teardownComplete)
				return false;
			DXVKInterop::GetSingleton()->ReleaseRetainedPresentResourcesAfterFSRSwapchainTeardown();
		}

		// Remove the outgoing WSI hook set before mapping the incoming one. This
		// keeps the interposer's hook table single-owner throughout a method switch.
		if (!wantDLSSG && !ReconcileFgFeatureLoad(
				sl::kFeatureDLSS_G, g_dlssgDesiredLoaded, g_dlssgCurrentlyLoaded))
			return false;
		if (!wantFSRFG && !ReconcileFgFeatureLoad(
				sl::kFeatureFSR_G, g_fsrfgDesiredLoaded, g_fsrfgCurrentlyLoaded))
			return false;
		if (wantDLSSG && !ReconcileFgFeatureLoad(
				sl::kFeatureDLSS_G, g_dlssgDesiredLoaded, g_dlssgCurrentlyLoaded))
			return false;
		if (wantFSRFG && !ReconcileFgFeatureLoad(
				sl::kFeatureFSR_G, g_fsrfgDesiredLoaded, g_fsrfgCurrentlyLoaded))
			return false;
		return true;
	}

	// DXVK uses the owner kind to preserve DLSS-G's present ID while still
	// stripping unsupported pNext structures from FSR's replacement swapchain.
	uint32_t DxvkFrameGenerationOwnsSwapchain(VkSwapchainKHR a_swapchain)
	{
		if (g_dlssgCurrentlyLoaded.load(std::memory_order_acquire))
			return 2u;
		if (g_sl.dispatchFaulted.load(std::memory_order_acquire))
			return g_fsrfgOwnsPresent.load(std::memory_order_acquire) ? 1u : 0u;
		if (!g_fsrfgOwnsPresent.load(std::memory_order_acquire) || !g_sl.slFSRFrameGenerationOwnsSwapchain)
			return 0u;

		bool ownsSwapchain = false;
		__try {
			ownsSwapchain = g_sl.slFSRFrameGenerationOwnsSwapchain(a_swapchain);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			g_sl.dispatchFaulted = true;
		}
		return ownsSwapchain ? 1u : 0u;
	}

	uint32_t EmitBridgedPresentMarker(bool a_begin)
	{
		if (!g_sl.slPCLSetMarker || g_sl.dispatchFaulted.load(std::memory_order_acquire))
			return 0;

		uint32_t frameId = 0;
		if (a_begin) {
			const uint32_t head = g_sl.presentMarkerHead.load(std::memory_order_relaxed);
			if (head == g_sl.presentMarkerTail.load(std::memory_order_acquire))
				return 0;
			frameId = g_sl.presentMarkerFrames[head % g_sl.presentMarkerFrames.size()].load(std::memory_order_relaxed);
			g_sl.presentMarkerHead.store(head + 1u, std::memory_order_release);
			g_sl.activePresentMarkerFrame.store(frameId, std::memory_order_release);
		} else {
			frameId = g_sl.activePresentMarkerFrame.exchange(0u, std::memory_order_acq_rel);
			if (!frameId)
				return 0;
		}

		__try {
			sl::FrameToken* token = nullptr;
			if (g_sl.slGetNewFrameToken(token, &frameId) == sl::Result::eOk && token)
				g_sl.slPCLSetMarker(a_begin ? sl::PCLMarker::ePresentStart : sl::PCLMarker::ePresentEnd, *token);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			g_sl.dispatchFaulted = true;
		}
		return frameId;
	}

	// Apply pending options immediately before the real, Streamline-interposed
	// vkQueuePresentKHR. This makes the mode change and the Present that consumes
	// it unambiguous and avoids racing DXVK's asynchronous presenter.
	void DxvkPresentBeginCallback(const DxvkPresentCallbackInfo* a_info)
	{
		if (!a_info || a_info->size < sizeof(DxvkPresentCallbackInfo) ||
			a_info->version != 1u || a_info->frameGenOwner != 2u) {
			logger::critical("[DLSSG-Ledger] rejected invalid DXVK present callback payload");
			g_sl.dispatchFaulted = true;
			return;
		}
		if (!g_dlssgCurrentlyLoaded.load(std::memory_order_acquire) ||
			!g_sl.slDLSSGSetOptions || g_sl.dispatchFaulted.load(std::memory_order_acquire))
			return;

		g_sl.dlssgApiMutex.lock();
		if (!g_dlssgCurrentlyLoaded.load(std::memory_order_acquire) ||
			!g_sl.slDLSSGSetOptions || g_sl.dispatchFaulted.load(std::memory_order_acquire)) {
			g_sl.dlssgApiMutex.unlock();
			return;
		}
		if (!g_sl.dlssgOptionsPending.load(std::memory_order_acquire)) {
			g_sl.dlssgApiMutex.unlock();
			const uint32_t markerFrame = EmitBridgedPresentMarker(true);
			g_sl.activeDxvkFrameId.store(a_info->frameId, std::memory_order_release);
			g_sl.activeSwapchainSerial.store(a_info->swapchainSerial, std::memory_order_release);
			g_sl.activePresentStartNs.store(PresentClockNs(), std::memory_order_release);
			if (g_sl.dlssgLedgerBudget.load(std::memory_order_acquire)) {
				logger::info("[DLSSG-Ledger] begin epoch={} dxvkFrame={} markerFrame={} swapchain={:#x}/{} presenter={:#x} image={} queue={:#x} waitGen={} pendingWaits={} markerDepth={} tid={}",
					g_sl.dlssgOptionsEpoch.load(std::memory_order_acquire), a_info->frameId, markerFrame,
					a_info->swapchain, a_info->swapchainSerial, a_info->presenter, a_info->imageIndex,
					a_info->queue, a_info->presentWaitGeneration, a_info->pendingPresentWaitCount,
					g_sl.presentMarkerTail.load(std::memory_order_acquire) - g_sl.presentMarkerHead.load(std::memory_order_acquire),
					GetCurrentThreadId());
			}
			return;
		}

		bool succeeded = false;
		__try {
			const sl::Result result = g_sl.slDLSSGSetOptions(g_sl.viewport, g_sl.dlssgPendingOptions);
			if (result == sl::Result::eOk) {
				succeeded = true;
				g_sl.dlssgModeOn.store(g_sl.dlssgPendingEnable, std::memory_order_release);
				logger::info("[Streamline] applied present-ordered DLSS-G mode={} numFrames={} render={}x{} display={}x{}",
					g_sl.dlssgPendingEnable, g_sl.dlssgPendingOptions.numFramesToGenerate,
					g_sl.dlssgPendingRenderW, g_sl.dlssgPendingRenderH,
					g_sl.dlssgPendingDisplayW, g_sl.dlssgPendingDisplayH);
			} else {
				logger::warn("[Streamline] present-thread slDLSSGSetOptions failed (result {})", static_cast<int>(result));
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			g_sl.dispatchFaulted = true;
			logger::error("[Streamline] present-thread DLSS-G SetOptions faulted - Streamline disabled for this session");
		}
		g_sl.dlssgOptionsPending.store(false, std::memory_order_release);
		if (!succeeded)
			g_sl.dlssgModeCached = false;
		g_sl.dlssgApiMutex.unlock();

		// The marker pair and the present must observe the same DLSS-G mode.
		// In particular, enabling after PresentStart can make the pacer wait for
		// work that was never scheduled for that frame.
		const uint32_t markerFrame = EmitBridgedPresentMarker(true);
		g_sl.activeDxvkFrameId.store(a_info->frameId, std::memory_order_release);
		g_sl.activeSwapchainSerial.store(a_info->swapchainSerial, std::memory_order_release);
		g_sl.activePresentStartNs.store(PresentClockNs(), std::memory_order_release);
		if (g_sl.dlssgLedgerBudget.load(std::memory_order_acquire)) {
			logger::info("[DLSSG-Ledger] begin epoch={} dxvkFrame={} markerFrame={} swapchain={:#x}/{} presenter={:#x} image={} queue={:#x} waitGen={} pendingWaits={} markerDepth={} tid={}",
				g_sl.dlssgOptionsEpoch.load(std::memory_order_acquire), a_info->frameId, markerFrame,
				a_info->swapchain, a_info->swapchainSerial, a_info->presenter, a_info->imageIndex,
				a_info->queue, a_info->presentWaitGeneration, a_info->pendingPresentWaitCount,
				g_sl.presentMarkerTail.load(std::memory_order_acquire) - g_sl.presentMarkerHead.load(std::memory_order_acquire),
				GetCurrentThreadId());
		}
	}

	// DXVK invokes this after the real, Streamline-interposed vkQueuePresentKHR
	// returns. NVIDIA requires slDLSSGGetState to run on that present thread.
	// In queue-parallel mode, associate the input-completion timeline with the
	// command-ring slot for this present. Slot selection later polls completion
	// without blocking; a Vulkan queue wait here could form a GPU queue cycle.
	void DxvkPresentCompletedCallback(const DxvkPresentCallbackInfo* a_info)
	{
		if (!a_info || a_info->size < sizeof(DxvkPresentCallbackInfo) ||
			a_info->version != 1u || a_info->frameGenOwner != 2u) {
			logger::critical("[DLSSG-Ledger] rejected invalid DXVK present completion payload");
			g_sl.dispatchFaulted = true;
			return;
		}
		const uint64_t beginNs = g_sl.activePresentStartNs.exchange(0u, std::memory_order_acq_rel);
		const uint64_t durationUs = beginNs ? (PresentClockNs() - beginNs) / 1000u : 0u;
		const uint64_t activeFrame = g_sl.activeDxvkFrameId.exchange(0u, std::memory_order_acq_rel);
		const uint64_t activeSwapchainSerial = g_sl.activeSwapchainSerial.exchange(0u, std::memory_order_acq_rel);
		if (!activeFrame)
			return;

		// Close the PCL present interval immediately after vkQueuePresentKHR.
		// Querying state inside that interval can make the pacer observe an
		// incomplete marker pair while it flushes the just-presented frame.
		const uint32_t markerFrame = EmitBridgedPresentMarker(false);
		const uint32_t budget = g_sl.dlssgLedgerBudget.load(std::memory_order_acquire);
		if (budget) {
			logger::info("[DLSSG-Ledger] end epoch={} dxvkFrame={} activeFrame={} markerFrame={} swapchain={:#x}/{} activeSerial={} image={} waitGen={} result={} presentUs={} tid={}",
				g_sl.dlssgOptionsEpoch.load(std::memory_order_acquire), a_info->frameId, activeFrame,
				markerFrame, a_info->swapchain, a_info->swapchainSerial, activeSwapchainSerial,
				a_info->imageIndex, a_info->presentWaitGeneration, a_info->presentResult, durationUs,
				GetCurrentThreadId());
			g_sl.dlssgLedgerBudget.fetch_sub(1u, std::memory_order_acq_rel);
		}
		if (activeFrame != a_info->frameId || activeSwapchainSerial != a_info->swapchainSerial) {
			logger::critical("[DLSSG-Ledger] present identity mismatch beginFrame={} endFrame={} beginSerial={} endSerial={}",
				activeFrame, a_info->frameId, activeSwapchainSerial, a_info->swapchainSerial);
			g_sl.dispatchFaulted = true;
		}

		if (!g_dlssgCurrentlyLoaded.load(std::memory_order_acquire) ||
			!g_sl.slDLSSGGetState || g_sl.dispatchFaulted.load(std::memory_order_acquire))
			return;

		g_sl.dlssgApiMutex.lock();
		if (!g_dlssgCurrentlyLoaded.load(std::memory_order_acquire) ||
			!g_sl.slDLSSGGetState || g_sl.dispatchFaulted.load(std::memory_order_acquire)) {
			g_sl.dlssgApiMutex.unlock();
			return;
		}
		__try {
			sl::DLSSGState state{};
			if (g_sl.slDLSSGGetState(g_sl.viewport, state, nullptr) == sl::Result::eOk) {
				if (g_sl.dlssgLedgerBudget.load(std::memory_order_acquire)) {
					logger::info("[DLSSG-Ledger] state completionFence={:#x} completionValue={} status={} presented={}",
						reinterpret_cast<uintptr_t>(state.inputsProcessingCompletionFence),
						state.lastPresentInputsProcessingCompletionFenceValue,
						static_cast<uint32_t>(state.status), state.numFramesActuallyPresented);
				}
				if (g_sl.dlssgModeOn.load(std::memory_order_acquire) &&
					state.inputsProcessingCompletionFence &&
					state.lastPresentInputsProcessingCompletionFenceValue) {
					const bool tracked = DXVKInterop::GetSingleton()->TrackInputCompletion(
						a_info->presentWaitGeneration,
						reinterpret_cast<VkSemaphore>(state.inputsProcessingCompletionFence),
						state.lastPresentInputsProcessingCompletionFenceValue);
					if (g_sl.dlssgLedgerBudget.load(std::memory_order_acquire))
						logger::info("[DLSSG-Ledger] completion tracked waitGen={} value={} tracked={}",
							a_info->presentWaitGeneration,
							state.lastPresentInputsProcessingCompletionFenceValue, tracked);
					if (!tracked) {
						g_sl.dispatchFaulted = true;
						logger::error("[Streamline] DLSS-G input completion could not be associated with its tagged slot; disabling Streamline for this session");
					}
				}
				// A successful state query after Present proves that the proxy swapchain
				// and its cloned buffers are ready, even while interpolation is off.
				g_sl.dlssgCloneTagsPrimed.store(true, std::memory_order_release);
				if (state.numFramesToGenerateMax > 0u) {
					const uint32_t previous = g_sl.dlssgMaxFramesToGenerate.exchange(
						state.numFramesToGenerateMax, std::memory_order_acq_rel);
					g_sl.dlssgDynamicSupported.store(
						state.bIsDynamicMFGSupported == sl::Boolean::eTrue, std::memory_order_release);
					if (!previous) {
						logger::info("[Streamline] DLSS-G numFramesToGenerateMax = {} (max {}x multiplier), DynamicMFG supported = {}",
							state.numFramesToGenerateMax, state.numFramesToGenerateMax + 1u,
							state.bIsDynamicMFGSupported == sl::Boolean::eTrue);
					}
				}

				if (g_sl.dlssgModeOn.load(std::memory_order_acquire)) {
					g_sl.frameGenerationMultiplier.store(
						std::max(state.numFramesActuallyPresented, 1u), std::memory_order_release);
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			g_sl.dispatchFaulted = true;
		}
		g_sl.dlssgApiMutex.unlock();
		g_sl.presentHeartbeatNs.store(PresentClockNs(), std::memory_order_release);
	}

	// Suppress exact known-benign diagnostics; pass all other messages through.
	bool IsBenignSLWarning(const char* a_msg)
	{
		if (!a_msg)
			return false;
		static constexpr const char* kBenign[] = {
			"setAsyncFrameMarker is not implemented",
			"is NOT supported, plugin will not function properly",
			"RSync will not run because it was not initialized",
			"Invalid backbuffer resource extent",
			"some DX/VK APIs were invoked before slInit",
			"reseting frame timer",
		};
		for (const char* needle : kBenign) {
			if (std::strstr(a_msg, needle))
				return true;
		}
		return false;
	}

	void LogCallback(sl::LogType a_type, const char* a_msg)
	{
		static const bool s_verbose = [] {
			char v[2] = {};
			return GetEnvironmentVariableA("CS_SL_VERBOSE", v, sizeof(v)) && v[0] == 0x31;
		}();
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
			logger::debug("[Streamline/SL] {}", a_msg);
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

void Streamline::PreloadInterposer()
{
	// Preload before DXVK creates VkInstance so its Vulkan loader aliases the interposer.
	if (disabledByConfig || g_sl.interposer)
		return;
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

// Probe with the system loader before slInit decides which FG plugin to load.
static bool ProbeDLSSGHardware()
{
	if (char v[2] = {}; GetEnvironmentVariableA("CS_FORCE_FSR_FG", v, sizeof(v)) && v[0] == '1') {
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
			if (enumDevices && enumExts) {
				uint32_t count = 0;
				enumDevices(instance, &count, nullptr);
				std::vector<VkPhysicalDevice> devices(count);
				enumDevices(instance, &count, devices.data());
				for (auto dev : devices) {
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
	logger::info("[Streamline] hardware probe: DLSS-G-class GPU (VK_NV_optical_flow) {}", found ? "present" : "absent");
	return found;
}

bool Streamline::Initialize()
{
	if (disabledByConfig)
		return false;
	if (triedInit)
		return initialized;
	triedInit = true;

	const auto slDir = GetStreamlineDir();
	if (slDir.empty()) {
		logger::warn("[Streamline] could not resolve plugin directory");
		return false;
	}

	const auto interposerPath = (slDir / L"sl.interposer.dll").wstring();
	if (!g_sl.interposer)
		g_sl.interposer = LoadLibraryExW(interposerPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!g_sl.interposer) {
		logger::info("[Streamline] sl.interposer.dll not present in '{}' — Streamline features disabled", slDir.string());
		return false;
	}

	const bool resolved =
		Resolve(g_sl.slInit, "slInit") &&
		Resolve(g_sl.slIsFeatureSupported, "slIsFeatureSupported") &&
		Resolve(g_sl.slGetNewFrameToken, "slGetNewFrameToken") &&
		Resolve(g_sl.slSetTagForFrame, "slSetTagForFrame") &&
		Resolve(g_sl.slSetConstants, "slSetConstants") &&
		Resolve(g_sl.slEvaluateFeature, "slEvaluateFeature") &&
		Resolve(g_sl.slGetFeatureFunction, "slGetFeatureFunction") &&
		Resolve(g_sl.slIsFeatureLoaded, "slIsFeatureLoaded");

	Resolve(g_sl.slSetFeatureLoaded, "slSetFeatureLoaded");
	if (!resolved) {
		FreeLibrary(g_sl.interposer);
		g_sl.interposer = nullptr;
		return false;
	}

	const auto slDirWide = slDir.wstring();
	const wchar_t* pluginPaths[] = { slDirWide.c_str() };
	// Request both supported frame-generation plugins so support probing, menu
	// population, and later runtime switching have access to both interfaces.
	// They must not both retain Vulkan WSI hooks once a swapchain exists; the
	// inactive plugin is disabled after probing and before the first swapchain.
	dlssgHardware = ProbeDLSSGHardware();

	std::vector<sl::Feature> featuresToLoad = { sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL,
		sl::kFeatureFSR, sl::kFeatureFSR_G, sl::kFeatureXeSS };
	if (dlssgHardware) {
		featuresToLoad.push_back(sl::kFeatureDLSS_G);
		g_dlssgCurrentlyLoaded.store(true, std::memory_order_release);
	}
	g_fsrfgCurrentlyLoaded.store(true, std::memory_order_release);

	sl::Preferences pref{};
	pref.renderAPI = sl::RenderAPI::eVulkan;
	pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	pref.featuresToLoad = featuresToLoad.data();
	pref.numFeaturesToLoad = static_cast<uint32_t>(featuresToLoad.size());
	pref.pathsToPlugins = pluginPaths;
	pref.numPathsToPlugins = 1;
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0";
	pref.projectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
	if (char v[2] = {}; GetEnvironmentVariableA("CS_SL_VERBOSE", v, sizeof(v)) && v[0] == 0x31)
		pref.logLevel = sl::LogLevel::eVerbose;
	else
		pref.logLevel = sl::LogLevel::eDefault;
	pref.logMessageCallback = &LogCallback;

	const sl::Result res = g_sl.slInit(pref, sl::kSDKVersion);
	if (res != sl::Result::eOk) {
		logger::warn("[Streamline] slInit failed (result {}) — Streamline features disabled", static_cast<int>(res));
		FreeLibrary(g_sl.interposer);
		g_sl.interposer = nullptr;
		return false;
	}

	initialized = true;
	StartDlssgWatchdog();
	logger::info("[Streamline] initialized on Vulkan (SDK {}.{}.{})",
		SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
	return true;
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
	featureFSR = supported(sl::kFeatureFSR);
	featureFSRFG = supported(sl::kFeatureFSR_G);

	// A requested plugin can be rejected during device startup even though it
	// was mapped at slInit. Seed reconciliation from Streamline's actual state;
	// otherwise unloading an already-rejected plugin fails and can mask the
	// other frame-generation method from the menu.
	const auto refreshLoadedState = [&](sl::Feature a_feature, bool a_supported,
									  std::atomic<bool>& a_current) {
		bool loaded = false;
		if (g_sl.slIsFeatureLoaded(a_feature, loaded) == sl::Result::eOk)
			a_current.store(loaded, std::memory_order_release);
		else if (!a_supported)
			a_current.store(false, std::memory_order_release);
	};
	refreshLoadedState(sl::kFeatureDLSS_G, featureDLSSG, g_dlssgCurrentlyLoaded);
	refreshLoadedState(sl::kFeatureFSR_G, featureFSRFG, g_fsrfgCurrentlyLoaded);

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

	HMODULE dxvkModule = GetModuleHandleW(L"dxvk_d3d11.dll");
	const bool frameGenerationInteropReady = dxvkModule &&
		GetProcAddress(dxvkModule, "dxvkRequestSwapchainRecreate") &&
		GetProcAddress(dxvkModule, "dxvkSetSyncPresent") &&
		GetProcAddress(dxvkModule, "dxvkGetPresenterSurfaceState") &&
		GetProcAddress(dxvkModule, "dxvkSetSwapchainTornDownCallback") &&
		GetProcAddress(dxvkModule, "dxvkSetFrameGenOwnershipQuery");
	const bool dlssgInteropReady = frameGenerationInteropReady &&
		GetProcAddress(dxvkModule, "dxvkSetPresentBeginCallback") &&
		GetProcAddress(dxvkModule, "dxvkSetPresentCompletedCallback");
	featureDLSSG = featureDLSSG && dlssgHardware && dlssgInteropReady &&
	               dxvk->FrameGenerationQueueInteropReady();
	featureFSRFG = featureFSRFG && frameGenerationInteropReady &&
	               dxvk->FrameGenerationQueueInteropReady();

	// Cache support while both requested plugins are still loaded, then leave
	// exactly one present owner enabled before swapchain creation. This preserves
	// both menu choices without stacking the FFX and DLSS-G WSI proxies. Runtime
	// switches repeat this operation in DXVK's no-swapchain callback.
	g_dlssgDesiredLoaded.store(featureDLSSG, std::memory_order_release);
	g_fsrfgDesiredLoaded.store(!featureDLSSG && featureFSRFG, std::memory_order_release);
	if (!ReconcileFgFeatureLoad(sl::kFeatureDLSS_G, g_dlssgDesiredLoaded, g_dlssgCurrentlyLoaded) ||
		!ReconcileFgFeatureLoad(sl::kFeatureFSR_G, g_fsrfgDesiredLoaded, g_fsrfgCurrentlyLoaded)) {
		featureDLSSG = false;
		featureFSRFG = false;
		logger::error("[Streamline] failed to isolate frame-generation plugins before swapchain creation");
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
		isRTXBelow40Series = isNvidiaGPU &&
		                     ((props.deviceID >= 0x2200 && props.deviceID <= 0x2600) ||   // RTX 30 (Ampere)
								(props.deviceID >= 0x1E00 && props.deviceID <= 0x1FFF));   // RTX 20 (Turing w/ RT)
		logger::info("[Streamline] GPU vendor=0x{:04X} device=0x{:04X} -> DLSS preset group: {}",
			props.vendorID, props.deviceID,
			isNvidiaGPU ? (isRTXBelow40Series ? "RTX 20/30 (J)" : "RTX 40+ (M)") : "non-NVIDIA (default)");
	}
}

static sl::FrameToken* TokenForFrame(uint32_t a_frameId)
{
	sl::FrameToken* token = nullptr;
	if (g_sl.slGetNewFrameToken(token, &a_frameId) != sl::Result::eOk)
		return nullptr;
	return token;
}

static sl::FrameToken* RenderFrameToken()
{
	return TokenForFrame(g_sl.renderFrameId);
}

static sl::Result cs_SetTagForFrame(sl::FrameToken& a_token, const sl::ViewportHandle& a_viewport,
	const sl::ResourceTag* a_tags, uint32_t a_tagCount, VkCommandBuffer a_commandBuffer)
{
	sl::Result result = sl::Result::eErrorExceptionHandler;
	__try {
		result = g_sl.slSetTagForFrame(a_token, a_viewport, a_tags, a_tagCount, a_commandBuffer);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
	}
	return result;
}

static sl::Result cs_EvaluateFeature(sl::Feature a_feature, sl::FrameToken& a_token,
	const sl::ViewportHandle& a_viewport, VkCommandBuffer a_commandBuffer)
{
	sl::Result result = sl::Result::eErrorExceptionHandler;
	__try {
		const sl::BaseStructure* inputs[] = { &a_viewport };
		result = g_sl.slEvaluateFeature(
			a_feature, a_token, inputs, static_cast<uint32_t>(std::size(inputs)), a_commandBuffer);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
	}
	return result;
}

static sl::Result cs_DiscardFSRFrameGenerationPreparedFrame(const sl::ViewportHandle& a_viewport)
{
	sl::Result result = sl::Result::eErrorNotInitialized;
	if (!g_sl.slFSRFrameGenerationDiscardPreparedFrame)
		return result;

	__try {
		result = g_sl.slFSRFrameGenerationDiscardPreparedFrame(a_viewport);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		result = sl::Result::eErrorExceptionHandler;
	}
	return result;
}

static uint32_t SimFrameId()
{
	return globals::state->frameCountAtomic.load(std::memory_order_relaxed) + 1;
}

void Streamline::BeginRenderFrame()
{
	g_sl.renderHeartbeatNs.store(PresentClockNs(), std::memory_order_release);
	if (g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire) &&
		!g_sl.dispatchFaulted.load(std::memory_order_acquire))
		(void)DiscardFSRFrameGenerationPreparedFrame();
	g_sl.renderFrameId = globals::state->frameCount;
	g_sl.dlssgTaggedThisFrame = false;
}

bool Streamline::DiscardFSRFrameGenerationPreparedFrame()
{
	if (!g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire))
		return true;
	if (!g_sl.slFSRFrameGenerationDiscardPreparedFrame ||
		g_sl.dispatchFaulted.load(std::memory_order_acquire))
		return false;

	const sl::ViewportHandle fgViewport{ 1 };
	const sl::Result result = cs_DiscardFSRFrameGenerationPreparedFrame(fgViewport);
	if (result == sl::Result::eErrorInvalidState) {
		DXVKInterop::GetSingleton()->QuarantineUnconsumedFSRPresentViews();
		return true;
	}
	if (result != sl::Result::eOk) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] FSR prepared-frame discard failed (result {})",
			static_cast<int>(result));
		return false;
	}

	DXVKInterop::GetSingleton()->NotifyFSRFrameConsumed();
	return true;
}

void Streamline::UpdateReflex(bool a_enable, bool a_boost, uint32_t a_frameLimitUs)
{
	if (!initialized || !featureReflex || g_sl.dispatchFaulted)
		return;

	const sl::ReflexMode mode = !a_enable ? sl::ReflexMode::eOff :
	                            a_boost   ? sl::ReflexMode::eLowLatencyWithBoost :
	                                        sl::ReflexMode::eLowLatency;

	__try {
		if (!g_sl.reflexCacheValid || g_sl.reflexCachedMode != mode || g_sl.reflexCachedFrameLimitUs != a_frameLimitUs) {
			sl::ReflexOptions options{};
			options.mode = mode;
			options.frameLimitUs = a_frameLimitUs;
			if (g_sl.slReflexSetOptions(options) == sl::Result::eOk) {
				g_sl.reflexCachedMode = mode;
				g_sl.reflexCachedFrameLimitUs = a_frameLimitUs;
				g_sl.reflexCacheValid = true;
			}
		}
		if (mode != sl::ReflexMode::eOff) {
			// PollInputDevices can run more than once per rendered frame.
			static uint32_t s_lastSleepFrame = UINT32_MAX;
			const uint32_t simFrame = SimFrameId();
			if (s_lastSleepFrame != simFrame) {
				s_lastSleepFrame = simFrame;
				if (sl::FrameToken* token = TokenForFrame(simFrame))
					g_sl.slReflexSleep(*token);
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] Reflex dispatch faulted — Streamline disabled for this session");
	}
}

void Streamline::SetPCLMarker(PclMarker a_marker)
{
	if (!initialized || !g_sl.slPCLSetMarker || g_sl.dispatchFaulted)
		return;

	// Emit SimulationStart once per simulated frame, not once per input poll.
	uint32_t simFrame = 0;
	if (a_marker == PclMarker::SimulationStart) {
		static uint32_t s_lastSimFrame = UINT32_MAX;
		simFrame = SimFrameId();
		if (s_lastSimFrame == simFrame)
			return;
		s_lastSimFrame = simFrame;
	}

	__try {
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
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] PCL marker faulted — Streamline disabled for this session");
	}
}

bool Streamline::QueueDLSSGPresentMarkers()
{
	if (!initialized || !g_sl.slPCLSetMarker || g_sl.dispatchFaulted.load(std::memory_order_acquire))
		return false;
	const uint32_t tail = g_sl.presentMarkerTail.load(std::memory_order_relaxed);
	const uint32_t head = g_sl.presentMarkerHead.load(std::memory_order_acquire);
	if (tail - head >= g_sl.presentMarkerFrames.size()) {
		logger::warn("[Streamline] Vulkan present-marker queue overflow; using app-thread markers");
		return false;
	}
	g_sl.presentMarkerFrames[tail % g_sl.presentMarkerFrames.size()].store(
		g_sl.renderFrameId, std::memory_order_relaxed);
	g_sl.presentMarkerTail.store(tail + 1u, std::memory_order_release);
	return true;
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
	a_consts.cameraNear = *globals::game::cameraNear;
	a_consts.cameraFar = *globals::game::cameraFar;

	auto viewMatrix = globals::game::frameBufferCached.GetCameraViewInverse().Transpose();
	auto cameraViewToClip = globals::game::frameBufferCached.GetCameraProjUnjittered().Transpose();

	a_consts.cameraMotionIncluded = sl::Boolean::eTrue;
	a_consts.cameraPinholeOffset = { 0.f, 0.f };
	a_consts.cameraRight = { viewMatrix._11, viewMatrix._12, viewMatrix._13 };
	a_consts.cameraUp = { viewMatrix._21, viewMatrix._22, viewMatrix._23 };
	a_consts.cameraFwd = { viewMatrix._31, viewMatrix._32, viewMatrix._33 };
	const auto cameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();
	a_consts.cameraPos = *reinterpret_cast<const sl::float3*>(&cameraPosAdjust);
	a_consts.cameraViewToClip = *reinterpret_cast<const sl::float4x4*>(&cameraViewToClip);
	a_consts.depthInverted = sl::Boolean::eFalse;

	sl::recalculateCameraMatrices(a_consts);

	// Translate between the current and previous camera-relative origins before reprojection.
	// Streamline applies jitter separately, so both matrices remain unjittered.
	Matrix curVP = globals::game::frameBufferCached.GetCameraViewProjUnjittered().Transpose();
	Matrix prevVP = globals::game::frameBufferCached.GetCameraPreviousViewProjUnjittered().Transpose();
	const auto& posAdj = globals::game::frameBufferCached.GetCameraPosAdjust();
	const auto& prevPosAdj = globals::game::frameBufferCached.GetCameraPreviousPosAdjust();
	Matrix camDelta = Matrix::CreateTranslation(posAdj.x - prevPosAdj.x, posAdj.y - prevPosAdj.y, posAdj.z - prevPosAdj.z);
	Matrix clipToPrevClip = curVP.Invert() * camDelta * prevVP;
	Matrix prevClipToClip = clipToPrevClip.Invert();
	a_consts.clipToPrevClip = *reinterpret_cast<const sl::float4x4*>(&clipToPrevClip);
	a_consts.prevClipToClip = *reinterpret_cast<const sl::float4x4*>(&prevClipToClip);

	a_consts.jitterOffset = { -a_jitterX, -a_jitterY };
	// Reset temporal history after leaving a loading screen.
	{
		static bool s_wasLoading = false;
		static uint32_t s_observedFrame = UINT32_MAX;
		static uint32_t s_resetFrame = UINT32_MAX;
		const bool loading = globals::state->isLoadingMenuOpen;
		if (s_observedFrame != g_sl.renderFrameId) {
			s_observedFrame = g_sl.renderFrameId;
			if (!loading && s_wasLoading)
				s_resetFrame = g_sl.renderFrameId;
			s_wasLoading = loading;
		}
		a_consts.reset = s_resetFrame == g_sl.renderFrameId ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	}
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
		ID3D11Resource* resource = a_res;
		a_dxvk->QuarantineResourcesAfterVulkanDestructionFault(&resource, 1);
		g_sl.dispatchFaulted = true;
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
			ID3D11Resource* resource = a_res;
			a_dxvk->QuarantineResourcesAfterVulkanDestructionFault(&resource, 1);
		}
		view = VK_NULL_HANDLE;
		g_sl.dispatchFaulted = true;
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

static bool cs_DestroyViews(DXVKInterop* a_dxvk, VkDevice a_device,
	PFN_vkDestroyImageView a_destroyImageView, VkImageView* a_views, uint32_t a_count,
	ID3D11Resource* const* a_resources = nullptr, uint32_t a_resourceCount = 0)
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
			g_sl.dispatchFaulted = true;
			if (a_dxvk)
				a_dxvk->QuarantineResourcesAfterVulkanDestructionFault(
					a_resources, a_resourceCount);
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
	if (!barrierAttempt.completed)
		g_sl.dispatchFaulted = true;
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
	if (!transaction)
		return false;

	a_tagResult = cs_SetTagForFrame(
		a_token, a_viewport, a_tags, a_tagCount, transaction.GetCommandBuffer());
	if (a_tagResult != sl::Result::eOk)
		return false;
	// Signal the slot semaphore so DXVK can add it to the matching present's
	// wait list. The presenter may use a different Vulkan queue and can otherwise
	// overtake this tag/copy submission even though it was queued first on the
	// graphics queue.
	if (!a_dxvk->SubmitFrameCommandBuffer(transaction, true)) {
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
	if (!a_transaction.SubmissionMayBeInFlight() &&
		!g_sl.dispatchFaulted.load(std::memory_order_acquire)) {
		const sl::Result discardResult = cs_DiscardFSRFrameGenerationPreparedFrame(a_viewport);
		if (discardResult == sl::Result::eOk || discardResult == sl::Result::eErrorInvalidState)
			return true;
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] failed to discard a partially accepted FSR-FG frame (result {})",
			static_cast<int>(discardResult));
	}

	a_dxvk->QueueResourcesForPresent(a_transaction, a_resources, a_resourceCount);
	a_dxvk->QuarantineViewsUntilFSRSwapchainTeardown(a_transaction, a_views, a_viewCount);
	return false;
}

static sl::Result cs_EvaluateFeatureCore(sl::Feature a_feature, const sl::ViewportHandle& a_viewport,
	ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut, ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_outputWidth, uint32_t a_outputHeight,
	float a_jitterX, float a_jitterY, ID3D11Resource* a_hudlessColor = nullptr,
	bool* a_outputReady = nullptr, bool* a_skipped = nullptr)
{
	if (a_outputReady)
		*a_outputReady = false;
	if (a_skipped)
		*a_skipped = false;

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
		if (s_evalFrameByVp[vpId] == g_sl.renderFrameId) {
			if (a_skipped)
				*a_skipped = true;
			return sl::Result::eOk;
		}
		if (s_constFrameByVp[vpId] != g_sl.renderFrameId) {
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
			s_constFrameByVp[vpId] = g_sl.renderFrameId;
			if (vpId == 0)
				g_sl.viewport0ConstantsFrame = g_sl.renderFrameId;
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
		g_sl.dispatchFaulted = true;
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
			dxvk->QuarantineResourcesAfterVulkanDestructionFault(
				resources, static_cast<uint32_t>(std::size(resources)));
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
	auto transaction = dxvk->BeginFrameCommandBuffer();
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
			dxvk->QuarantineResourcesAfterVulkanDestructionFault(
				resources, static_cast<uint32_t>(std::size(resources)));
			std::fill(std::begin(views), std::end(views), VK_NULL_HANDLE);
			return sl::Result::eErrorExceptionHandler;
		}

		if (dxvk->SubmitFrameCommandBuffer(transaction)) {
			if (a_feature == sl::kFeatureFSR_G) {
				dxvk->QueueResourcesForPresent(
					transaction, resources, static_cast<uint32_t>(std::size(resources)));
				dxvk->QueueViewsForFSRPresent(transaction, views, static_cast<uint32_t>(nv));
			} else {
				dxvk->QueueResourcesForDeferredRelease(
					transaction, resources, static_cast<uint32_t>(std::size(resources)));
				dxvk->QueueViewsForDeferredDelete(transaction, views, static_cast<uint32_t>(nv));
			}
			if (a_outputReady)
				*a_outputReady = true;
			if (vpId < 2)
				s_evalFrameByVp[vpId] = g_sl.renderFrameId;
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
	if (!initialized || !featureDLSS || g_sl.dispatchFaulted)
		return result;
	if (!a_colorIn || !a_colorOut || !a_depth || !a_motionVectors)
		return result;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady())
		return result;

	__try {
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

		const sl::Result optionsResult = g_sl.slDLSSSetOptions(g_sl.viewport, options);
		if (optionsResult != sl::Result::eOk) {
			logger::error("[Streamline] DLSS options failed (result {})", static_cast<int>(optionsResult));
			return result;
		}

		const sl::Result evalRes = cs_EvaluateFeatureCore(sl::kFeatureDLSS, g_sl.viewport,
			a_colorIn, a_colorOut, a_depth, a_motionVectors,
			a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY,
			nullptr, &outputReady, &evaluationSkipped);
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
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] DLSS dispatch faulted — Streamline disabled for this session");
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
	if (!initialized || !featureXeSS || g_sl.dispatchFaulted)
		return result;
	if (!a_colorIn || !a_colorOut || !a_depth || !a_motionVectors)
		return result;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady())
		return result;

	__try {
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
		const sl::Result optionsResult = g_sl.slXeSSSetOptions(g_sl.viewport, xessOpts);
		if (optionsResult != sl::Result::eOk) {
			logger::error("[Streamline] XeSS options failed (result {})", static_cast<int>(optionsResult));
			return result;
		}

		const sl::Result evalRes = cs_EvaluateFeatureCore(sl::kFeatureXeSS, g_sl.viewport,
			a_colorIn, a_colorOut, a_depth, a_motionVectors,
			a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY,
			nullptr, &outputReady, &evaluationSkipped);
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
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] XeSS dispatch faulted — Streamline disabled for this session");
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
	if (!initialized || !featureFSR || g_sl.dispatchFaulted)
		return result;
	if (!a_colorIn || !a_colorOut || !a_depth || !a_motionVectors)
		return result;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady())
		return result;

	__try {
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
		const sl::Result optionsResult = g_sl.slFSRSetOptions(g_sl.viewport, fsrOpts);
		if (optionsResult != sl::Result::eOk) {
			logger::error("[Streamline] FSR options failed (result {})", static_cast<int>(optionsResult));
			return result;
		}

		const sl::Result evalRes = cs_EvaluateFeatureCore(sl::kFeatureFSR, g_sl.viewport,
			a_colorIn, a_colorOut, a_depth, a_motionVectors,
			a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY,
			nullptr, &outputReady, &evaluationSkipped);
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
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] FSR dispatch faulted — Streamline disabled for this session");
	}
	return result;
}

bool Streamline::EvaluateFSRFrameGen(ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_hudlessColor,
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_outputWidth, uint32_t a_outputHeight,
	float a_jitterX, float a_jitterY)
{
	// Isolate FSR frame-generation preparation from viewport 0 upscaling tags and constants.
	if (!initialized || !featureFSRFG || g_sl.dispatchFaulted)
		return false;
	if (!a_depth || !a_motionVectors || !a_hudlessColor)
		return false;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady())
		return false;

	bool evaluationSubmitted = false;
	bool accepted = false;
	__try {
		const sl::ViewportHandle fgViewport{ 1 };
		const sl::Result evalRes = cs_EvaluateFeatureCore(sl::kFeatureFSR_G, fgViewport,
			nullptr, nullptr, a_depth, a_motionVectors,
			a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY,
			a_hudlessColor, &evaluationSubmitted);
		accepted = evalRes == sl::Result::eOk && evaluationSubmitted;

		static sl::Result s_loggedRes = sl::Result::eErrorNotInitialized;
		if (evalRes != s_loggedRes) {
			s_loggedRes = evalRes;
			logger::info("[Streamline] FSR FG-prepare result={} render={}x{}", static_cast<int>(evalRes), a_renderWidth, a_renderHeight);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] FSR FG-prepare faulted — Streamline disabled for this session");
	}
	return accepted;
}

bool Streamline::SetDLSSGMode(bool a_enable, uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_displayWidth, uint32_t a_displayHeight,
	uint32_t a_numFramesToGenerate, bool a_autoMode, bool a_dynamic, float a_dynamicTargetFps)
{
	if (!initialized || !featureDLSSG || g_sl.dispatchFaulted)
		return false;

	// Do not call the options entry point while DLSS-G is runtime-unloaded.
	if (!g_dlssgCurrentlyLoaded.load(std::memory_order_acquire))
		return false;
	// Starting interpolation without matching current-frame constants and real
	// input tags can wedge Streamline's Vulkan pacer on its first flush.
	if (a_enable && (!g_sl.dlssgCloneTagsPrimed.load(std::memory_order_acquire) ||
		!g_sl.dlssgTaggedThisFrame || g_sl.viewport0ConstantsFrame != g_sl.renderFrameId))
		return false;

	// Clamp the requested multiplier to the reported hardware limit.
	const uint32_t maxFrames = g_sl.dlssgMaxFramesToGenerate.load(std::memory_order_acquire);
	uint32_t numFrames = a_numFramesToGenerate < 1u ? 1u : a_numFramesToGenerate;
	if (maxFrames > 0u && numFrames > maxFrames)
		numFrames = maxFrames;

	const bool dynamicResolution = a_renderWidth < a_displayWidth || a_renderHeight < a_displayHeight;
	{
		std::lock_guard lock(g_sl.dlssgApiMutex);
		// The cache represents the latest published request; the present callback
		// updates dlssgModeOn only after Streamline accepts it. Keep the cache and
		// pending-option check under the same lock as the present callback.
		const bool changed = !(g_sl.dlssgModeCached &&
			g_sl.dlssgCachedNumFrames == numFrames && g_sl.dlssgCachedAuto == a_autoMode &&
			g_sl.dlssgCachedDynamic == a_dynamic && g_sl.dlssgCachedDynamicFps == a_dynamicTargetFps &&
			g_sl.dlssgCachedRenderW == a_renderWidth && g_sl.dlssgCachedRenderH == a_renderHeight &&
			g_sl.dlssgCachedDisplayW == a_displayWidth && g_sl.dlssgCachedDisplayH == a_displayHeight &&
			g_sl.dlssgPendingEnable == a_enable);
		if (!changed)
			return !g_sl.dlssgOptionsPending.load(std::memory_order_acquire) &&
			       g_sl.dlssgModeOn.load(std::memory_order_acquire) == a_enable;

		// Drain DXVK's asynchronous presenter only for this option transition.
		// The present-thread callback is the acknowledgment boundary; the
		// controller restores asynchronous DLSS-G presentation afterward.
		PushDxvkPresentQueueDepth(0u);

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
		if (dynamicResolution) {
			options.flags |= sl::DLSSGFlags::eDynamicResolutionEnabled;
			options.dynamicResWidth = a_renderWidth;
			options.dynamicResHeight = a_renderHeight;
		}
		options.mvecDepthWidth = a_displayWidth;
		options.mvecDepthHeight = a_displayHeight;
		options.colorWidth = a_displayWidth;
		options.colorHeight = a_displayHeight;
		// DXVK presents on a dedicated Vulkan queue. Let Streamline block that
		// queue until its input reads complete instead of manually inserting the
		// eBlockNoClientQueues completion fence into the graphics queue. An
		// immediate graphics-queue wait can be ordered before SL's internal signal
		// submission and permanently wedge both queues.
		// Vulkan queue-parallel mode keeps Streamline from parking a client queue
		// inside the driver. Completion is observed on the present thread below;
		// any required reuse wait must be applied at the resource reuse point, not
		// immediately submitted ahead of Streamline's signal-producing work.
		options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockNoClientQueues;
		g_sl.dlssgPendingOptions = options;
		g_sl.dlssgOptionsPending.store(true, std::memory_order_release);
		g_sl.dlssgPendingEnable = a_enable;
		g_sl.dlssgPendingRenderW = a_renderWidth;
		g_sl.dlssgPendingRenderH = a_renderHeight;
		g_sl.dlssgPendingDisplayW = a_displayWidth;
		g_sl.dlssgPendingDisplayH = a_displayHeight;
		g_sl.dlssgOptionsEpoch.fetch_add(1u, std::memory_order_acq_rel);
		g_sl.dlssgLedgerBudget.store(32u, std::memory_order_release);
		g_sl.dlssgModeCached = true;
		g_sl.dlssgCachedNumFrames = numFrames;
		g_sl.dlssgCachedAuto = a_autoMode;
		g_sl.dlssgCachedDynamic = a_dynamic;
		g_sl.dlssgCachedDynamicFps = a_dynamicTargetFps;
		g_sl.dlssgCachedRenderW = a_renderWidth;
		g_sl.dlssgCachedRenderH = a_renderHeight;
		g_sl.dlssgCachedDisplayW = a_displayWidth;
		g_sl.dlssgCachedDisplayH = a_displayHeight;
	}
	logger::info("[Streamline] queued present-ordered DLSS-G mode={} ({}) numFrames={} targetFps={} (max {}) render={}x{} display={}x{} drs={}", a_enable,
		!a_enable ? "off" : a_dynamic ? "dynamic" : a_autoMode ? "auto" : "on", numFrames, a_dynamicTargetFps, maxFrames,
		a_renderWidth, a_renderHeight, a_displayWidth, a_displayHeight, dynamicResolution);
	// Delivery completes in DxvkPresentBeginCallback. Returning false keeps the
	// controller from draining/unloading the device before eOff has taken effect.
	return false;
}

bool Streamline::SetFSRFrameGen(bool a_enable, bool a_hdr,
	bool a_debugView, bool a_debugTearLines, bool a_debugPacingLines, bool a_onlyPresentGenerated)
{
	// The caller retries until the runtime-loaded plugin accepts the option.
	if (!initialized || !featureFSRFG || !g_sl.slFSRFrameGenerationSetOptions || g_sl.dispatchFaulted)
		return false;
	if (!g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire))
		return false;

	bool ok = false;
	__try {
		sl::FSRFrameGenOptions options{};
		options.enabled = a_enable ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.colorBuffersHDR = a_hdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.debugView = a_debugView ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.debugTearLines = a_debugTearLines ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.debugPacingLines = a_debugPacingLines ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.onlyPresentGenerated = a_onlyPresentGenerated ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		const sl::Result res = g_sl.slFSRFrameGenerationSetOptions(g_sl.viewport, options);
		if (res != sl::Result::eOk) {
			g_sl.dispatchFaulted = true;
			logger::error("[Streamline] slFSRFrameGenerationSetOptions failed (result {})", static_cast<int>(res));
		} else {
			ok = true;
			g_fsrfgOwnsPresent.store(a_enable, std::memory_order_release);
			if (!a_enable)
				g_sl.frameGenerationMultiplier.store(1, std::memory_order_release);
			logger::info("[Streamline] FSR frame generation {}", a_enable ? "enabled" : "disabled");
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] FSR SetFrameGen faulted — Streamline disabled for this session");
	}
	return ok;
}

void Streamline::CaptureFSRFrameGenState()
{
	if (!initialized || !featureFSRFG || !g_sl.slFSRGetFrameGenState || g_sl.dispatchFaulted)
		return;
	__try {
		sl::FSRFrameGenState state{};
		if (g_sl.slFSRGetFrameGenState(g_sl.viewport, state) == sl::Result::eOk) {
			g_sl.frameGenerationMultiplier.store(
				std::max(state.numFramesActuallyPresented, 1u), std::memory_order_release);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
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

bool Streamline::IsDLSSGDynamicSupported() const
{
	return g_sl.dlssgDynamicSupported.load(std::memory_order_acquire);
}

bool Streamline::IsDLSSGFrameReady() const
{
	return g_sl.dlssgCloneTagsPrimed.load(std::memory_order_acquire) &&
	       g_sl.dlssgTaggedThisFrame &&
	       g_sl.viewport0ConstantsFrame == g_sl.renderFrameId;
}

bool Streamline::IsDLSSGOptionsPending() const
{
	return g_sl.dlssgOptionsPending.load(std::memory_order_acquire);
}

void Streamline::TagDLSSGResources(ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_hudlessColor, uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_displayWidth, uint32_t a_displayHeight)
{
	if (!initialized || !featureDLSSG || g_sl.dispatchFaulted)
		return;
	if (!a_depth || !a_motionVectors)
		return;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady())
		return;
	if (!g_sl.dlssgCloneTagsPrimed.load(std::memory_order_acquire)) {
		ClearDLSSGTags();
		return;
	}

	__try {
		sl::FrameToken* token = RenderFrameToken();
		if (!token)
			return;

		VkDevice vkDevice = dxvk->GetDevice();
		const cs_VulkanProcAttempt createProcAttempt = cs_GetDeviceProcAddrSEH(
			dxvk->GetDeviceProcAddr(), vkDevice, "vkCreateImageView");
		const cs_VulkanProcAttempt destroyProcAttempt = cs_GetDeviceProcAddrSEH(
			dxvk->GetDeviceProcAddr(), vkDevice, "vkDestroyImageView");
		if (createProcAttempt.exceptionCode || destroyProcAttempt.exceptionCode) {
			g_sl.dispatchFaulted = true;
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
			dxvk->QuarantineResourcesAfterVulkanDestructionFault(
				resources, static_cast<uint32_t>(std::size(resources)));
			std::fill(std::begin(views), std::end(views), VK_NULL_HANDLE);
		};

		const auto makeResource = [&](ID3D11Resource* a_res, sl::Resource& a_out,
			                          sl::SubresourceRange& a_subresource) {
			VkImage image = VK_NULL_HANDLE;
			VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			if (!vkCreateImageView || viewCount >= std::size(views))
				return false;
			const cs_GetVkImageAttempt imageAttempt =
				cs_GetVkImageSEH(dxvk, a_res, &image, &layout, &info);
			if (imageAttempt.exceptionCode) {
				viewCreationTerminalFault = true;
				ID3D11Resource* resource = a_res;
				dxvk->QuarantineResourcesAfterVulkanDestructionFault(&resource, 1);
				g_sl.dispatchFaulted = true;
				logger::error("[Streamline] DLSS-G DXVK image interop faulted (SEH {:#x})",
					imageAttempt.exceptionCode);
				return false;
			}
			if (!imageAttempt.succeeded || image == VK_NULL_HANDLE)
				return false;
			VkImageViewCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			ci.image = image;
			ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
			ci.format = info.format;
			ci.subresourceRange.aspectMask = cs_ImageAspect(info.format);
			ci.subresourceRange.levelCount = 1;
			ci.subresourceRange.layerCount = 1;
			VkImageView& view = views[viewCount];
			const cs_VulkanResultAttempt createAttempt =
				cs_CreateImageViewSEH(vkCreateImageView, vkDevice, &ci, &view);
			if (createAttempt.exceptionCode || createAttempt.result != VK_SUCCESS || view == VK_NULL_HANDLE) {
				if (createAttempt.exceptionCode || view != VK_NULL_HANDLE) {
					viewCreationTerminalFault = true;
					ID3D11Resource* resource = a_res;
					dxvk->QuarantineResourcesAfterVulkanDestructionFault(&resource, 1);
				}
				view = VK_NULL_HANDLE;
				g_sl.dispatchFaulted = true;
				logger::error("[Streamline] DLSS-G image-view creation failed (result {}, SEH {:#x})",
					static_cast<int>(createAttempt.result), createAttempt.exceptionCode);
				return false;
			}
			++viewCount;
			a_out = sl::Resource{ sl::ResourceType::eTex2d, image, nullptr, view, static_cast<uint32_t>(layout) };
			// Resource dimensions describe the image; tag extents describe the valid subrect.
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
		};

		sl::Resource depthRes{}, mvecRes{};
		sl::SubresourceRange depthRange{}, mvecRange{}, hudlessRange{};
		if (!makeResource(a_depth, depthRes, depthRange) ||
			!makeResource(a_motionVectors, mvecRes, mvecRange)) {
			abandonViewsAfterCreationFailure();
			return;
		}

		sl::Extent extent{};
		extent.width = a_renderWidth;
		extent.height = a_renderHeight;

		sl::ResourceTag tags[3];
		uint32_t tagCount = 0;
		tags[tagCount++] = { &depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &extent };
		tags[tagCount++] = { &mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &extent };

		// HUD-less color uses display dimensions rather than the render subrect.
		sl::Extent displayExtent{};
		displayExtent.width = a_displayWidth;
		displayExtent.height = a_displayHeight;
		sl::Resource hudlessRes{};
		const uint32_t viewsBeforeHudless = viewCount;
		if (a_hudlessColor) {
			if (makeResource(a_hudlessColor, hudlessRes, hudlessRange)) {
				tags[tagCount++] = { &hudlessRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &displayExtent };
			} else if (g_sl.dispatchFaulted.load(std::memory_order_acquire) ||
				viewCount != viewsBeforeHudless) {
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
		} else {
			if (!lifetimesRetained)
				destroyViews();
			logger::error("[Streamline] DLSS-G resource tag submission failed (result {})",
				static_cast<int>(tagResult));
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] DLSS-G tag faulted — Streamline disabled for this session");
	}
}

void Streamline::ClearDLSSGTags()
{
	if (!initialized || !featureDLSSG || g_sl.dispatchFaulted)
		return;

	__try {
		sl::FrameToken* token = RenderFrameToken();
		if (!token)
			return;

		// Null tags force passthrough when interpolation inputs are unavailable.
		sl::ResourceTag tags[] = {
			sl::ResourceTag{ nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, nullptr },
			sl::ResourceTag{ nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, nullptr },
			sl::ResourceTag{ nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, nullptr },
		};
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
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] DLSS-G clear-tags faulted — Streamline disabled for this session");
	}
}

bool Streamline::EnsureDLSSGPresentTag()
{
	// Supply passthrough tags when the render pass did not provide interpolation inputs.
	if (!initialized || !featureDLSSG || g_sl.dispatchFaulted)
		return false;
	if (!g_sl.dlssgTaggedThisFrame)
		ClearDLSSGTags();
	return g_sl.dlssgTaggedThisFrame;
}

void Streamline::RegisterDxvkOwnershipPredicate()
{
	// Streamline-owned swapchains must bypass DXVK's present-wait worker.
	HMODULE dxvkModule = GetModuleHandleW(L"dxvk_d3d11.dll");
	if (!dxvkModule) {
		logger::warn("[Streamline] DXVK module not loaded — cannot register ownership predicate");
		return;
	}
	using SetQueryFn = void (*)(uint32_t (*)(VkSwapchainKHR));
	auto setQuery = reinterpret_cast<SetQueryFn>(GetProcAddress(dxvkModule, "dxvkSetFrameGenOwnershipQuery"));
	if (!setQuery) {
		logger::warn("[Streamline] dxvkSetFrameGenOwnershipQuery not found in DXVK module");
		return;
	}
	setQuery(&DxvkFrameGenerationOwnsSwapchain);
	logger::info("[Streamline] registered DXVK frame-generation ownership predicate");

	using PresentCallbackFn = void (*)(const DxvkPresentCallbackInfo*);
	using SetPresentCompletedFn = void (*)(PresentCallbackFn);
	using SetPresentBeginFn = void (*)(PresentCallbackFn);
	auto setPresentBegin = reinterpret_cast<SetPresentBeginFn>(
		GetProcAddress(dxvkModule, "dxvkSetPresentBeginCallback"));
	auto setPresentCompleted = reinterpret_cast<SetPresentCompletedFn>(
		GetProcAddress(dxvkModule, "dxvkSetPresentCompletedCallback"));
	if (setPresentBegin && setPresentCompleted) {
		setPresentBegin(&DxvkPresentBeginCallback);
		setPresentCompleted(&DxvkPresentCompletedCallback);
		logger::info("[Streamline] registered DXVK Vulkan present-thread options/state callbacks");
	} else {
		logger::warn("[Streamline] DXVK DLSS-G present-thread bridge unavailable");
	}

	// Streamline features may only be loaded or unloaded while no swapchain exists.
	using SetTornDownFn = void (*)(bool (*)());
	if (auto setTornDown = reinterpret_cast<SetTornDownFn>(GetProcAddress(dxvkModule, "dxvkSetSwapchainTornDownCallback"))) {
		setTornDown(&DxvkSwapchainTornDownCallback);
		logger::info("[Streamline] registered DXVK swapchain-torn-down callback");
	} else {
		logger::warn("[Streamline] dxvkSetSwapchainTornDownCallback not found — frame-generation switching disabled");
	}

}

bool Streamline::HasDispatchFaulted() const
{
	return g_sl.dispatchFaulted.load(std::memory_order_acquire);
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

void Streamline::RequestDxvkSwapchainRecreate(const char* a_reason)
{
	// Recreate the Vulkan swapchain to apply runtime feature load changes.
	static auto requestRecreate = []() -> void (*)() {
		HMODULE dxvkModule = GetModuleHandleW(L"dxvk_d3d11.dll");
		if (!dxvkModule)
			return nullptr;
		return reinterpret_cast<void (*)()>(GetProcAddress(dxvkModule, "dxvkRequestSwapchainRecreate"));
	}();
	if (requestRecreate) {
		requestRecreate();
		logger::info("[Streamline] requested DXVK swapchain recreate ({})", a_reason);
	} else {
		logger::warn("[Streamline] dxvkRequestSwapchainRecreate not found — {} cannot take effect", a_reason);
	}
}

void Streamline::PushDxvkSyncPresent(bool a_sync)
{
	static std::atomic<int> s_applied{ -1 };
	const int requested = a_sync ? 1 : 0;
	if (s_applied.load(std::memory_order_acquire) == requested)
		return;

	static auto setSync = []() -> void (*)(uint32_t) {
		HMODULE dxvkModule = GetModuleHandleW(L"dxvk_d3d11.dll");
		if (!dxvkModule)
			return nullptr;
		return reinterpret_cast<void (*)(uint32_t)>(GetProcAddress(dxvkModule, "dxvkSetSyncPresent"));
	}();
	if (setSync) {
		setSync(a_sync ? 1u : 0u);
		s_applied.store(requested, std::memory_order_release);
		logger::info("[Streamline] DXVK synchronous present {}", a_sync ? "enabled" : "disabled");
	} else {
		static bool s_warned = false;
		if (!s_warned) {
			s_warned = true;
			logger::warn("[Streamline] dxvkSetSyncPresent not found - synchronous present control inactive");
		}
	}
}

void Streamline::PushDxvkPresentQueueDepth(uint32_t a_depth)
{
	static std::atomic<uint32_t> s_applied{ UINT32_MAX - 1u };
	if (s_applied.load(std::memory_order_acquire) == a_depth)
		return;

	static auto setDepth = []() -> void (*)(uint32_t) {
		HMODULE dxvkModule = GetModuleHandleW(L"dxvk_d3d11.dll");
		return dxvkModule ? reinterpret_cast<void (*)(uint32_t)>(
			GetProcAddress(dxvkModule, "dxvkSetPresentQueueDepth")) : nullptr;
	}();
	if (setDepth) {
		setDepth(a_depth);
		s_applied.store(a_depth, std::memory_order_release);
		if (a_depth == UINT32_MAX)
			logger::info("[Streamline] DXVK present queue depth unrestricted");
		else
			logger::info("[Streamline] DXVK present queue depth set to {}", a_depth);
	} else {
		// Preserve compatibility with builds predating bounded presentation.
		PushDxvkSyncPresent(a_depth == 0u);
	}
}

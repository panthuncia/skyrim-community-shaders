#pragma once

#include <cstdint>
#include <d3d11.h>
#include <string>
#include <vector>

// Streamline and community upscalers. DXVK mode: on DXVK's Vulkan device through full interposition. Native mode: on the native
// device (Vulkan or D3D12), which BasicRHI creates through Streamline's manual hooking (DeviceCreateInfo::streamlineHost), with the
// evaluate as a pass of the post stage (Upscaling's evaluate pass, ORGUpscaling::QueueEvaluate).

class Streamline
{
public:
	static Streamline* GetSingleton();

	/** @brief Maps the interposer before DXVK creates its Vulkan instance. */
	void PreloadInterposer();

	/** @brief Initializes Streamline's Vulkan backend. */
	bool Initialize();

	/** @brief Resolves feature support after the DXVK device is available. */
	void SetVulkanDevice();

	/**
	 * @brief Native mode, before the native device exists: loads the interposer and initializes Streamline for the native backend
	 * (manual hooking). a_adapterLuid names the adapter the device is made on: DLSS-G loads only when that adapter has the
	 * optical flow hardware. False, logged, when Streamline cannot run; the device is then made without it.
	 */
	bool InitializeForNative(uint64_t a_adapterLuid);

	/**
	 * @brief Native mode, after the device and before the first swap chain: unloads DLSS-G, which would otherwise attach to a swap
	 * chain made without a presentation layer. The frame generation controller loads it again through a presentation replacement.
	 */
	void UnloadDLSSGBeforeFirstSwapchain();

	/** @brief The interposer CS loaded (an HMODULE), after Initialize. Null before or without it. */
	[[nodiscard]] void* InterposerModule() const;
	/** @brief The sl::Feature values slInit loaded. */
	[[nodiscard]] const std::vector<uint32_t>& LoadedFeatures() const { return loadedFeatures; }
	/** @brief The plugins' directory (sl.common.dll's), after Initialize. */
	[[nodiscard]] const std::wstring& PluginDirectory() const { return pluginDirectory; }

	/**
	 * @brief Native mode, once the native device exists: resolves feature support on its adapter (a_vkPhysicalDevice on Vulkan, else
	 * the adapter a_luid names).
	 */
	void SetNativeDevice(void* a_vkPhysicalDevice, uint64_t a_luid);

	/** @brief Returns whether feature support is final for this session. */
	[[nodiscard]] bool IsFeatureSupportResolved() const { return vulkanDeviceSet || unavailable; }
	/** @brief Whether a guarded Streamline call faulted and frame generation must be torn down. */

	/** @brief Disables interposition when no Streamline feature is configured. */
	void MarkUnavailable() { unavailable = true; }
	[[nodiscard]] bool IsUnavailable() const { return unavailable; }
	[[nodiscard]] bool IsDLSSSupported() const { return featureDLSS; }
	[[nodiscard]] bool IsReflexSupported() const { return featureReflex; }
	[[nodiscard]] bool IsDLSSGSupported() const { return featureDLSSG; }
	[[nodiscard]] bool IsXeSSSupported() const { return featureXeSS; }
	/** @brief True when XeSS would use its XMX path (Intel Arc) rather than the softer DP4a fallback. */
	[[nodiscard]] bool IsXeSSHardwareAccelerated() const { return featureXeSS && isIntelGPU; }
	[[nodiscard]] bool IsFSRSupported() const { return featureFSR; }
	[[nodiscard]] bool IsFSRFGSupported() const { return featureFSRFG; }

	/** @brief Outcome of one regular upscaler evaluation. */
	enum class EvaluationResult : uint8_t
	{
		kReady,
		kSkipped,
		kFailed,
	};

	[[nodiscard]] EvaluationResult EvaluateDLSS(ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut,
		ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
		uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_outputWidth, uint32_t a_outputHeight,
		uint32_t a_qualityMode,
		float a_jitterX, float a_jitterY);

	[[nodiscard]] EvaluationResult EvaluateXeSS(ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut,
		ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
		uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_outputWidth, uint32_t a_outputHeight,
		uint32_t a_qualityMode, float a_sharpness,
		float a_jitterX, float a_jitterY);

	[[nodiscard]] EvaluationResult EvaluateFSR(ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut,
		ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
		uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_outputWidth, uint32_t a_outputHeight,
		uint32_t a_qualityMode, float a_sharpness,
		float a_jitterX, float a_jitterY);

	/** @brief Prepares FSR frame generation independently of the active upscaler. */
	[[nodiscard]] bool EvaluateFSRFrameGen(ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
		ID3D11Resource* a_hudlessColor,
		uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_outputWidth, uint32_t a_outputHeight,
		float a_jitterX, float a_jitterY, bool a_depthCopy);

	[[nodiscard]] bool SetFSRFrameGen(bool a_enable, bool a_hdr,
		bool a_debugView = false, bool a_debugTearLines = false, bool a_debugPacingLines = false,
		bool a_onlyPresentGenerated = false);

	void CaptureFSRFrameGenState();

	/** @brief Updates Reflex and its optional frame-limit interval in microseconds. */
	void UpdateReflex(bool a_enable, bool a_boost, uint32_t a_frameLimitUs = 0);

	enum class PclMarker : uint32_t
	{
		SimulationStart = 0,
		SimulationEnd = 1,
		RenderSubmitStart = 2,
		RenderSubmitEnd = 3,
		PresentStart = 4,
		PresentEnd = 5,
		TriggerFlash = 7,
		PCLatencyPing = 8,
	};

	void SetPCLMarker(PclMarker a_marker);

	/** @brief Updates DLSS-G mode and generated-frame count. */
	bool SetDLSSGMode(bool a_enable, uint32_t a_displayWidth, uint32_t a_displayHeight,
		uint32_t a_numFramesToGenerate = 1, bool a_autoMode = false, bool a_dynamic = false,
		float a_dynamicTargetFps = 0.0f);

	/** @brief Establishes the Streamline frame ID at render-frame start. */
	void BeginRenderFrame();
	/** @brief Discards any prepared FSR frame that did not reach Present. */
	[[nodiscard]] bool DiscardFSRFrameGenerationPreparedFrame();

	/** @brief Queries and caches DLSS-G capabilities on the present thread. */
	void QueryDLSSGCapabilities();

	/** @brief Updates DLSS-G state after the real present completes. */
	void CaptureDLSSGPresentState();
	[[nodiscard]] uint32_t GetDLSSGMaxFramesToGenerate() const;

	/** @brief Returns the latest number of frames presented per rendered frame. */
	[[nodiscard]] uint32_t GetFrameGenerationMultiplier() const;
	/** @brief Running total of frames the FSR-FG swapchain has presented; difference for the true rate. */
	[[nodiscard]] uint64_t GetTotalPresentedFrames() const;
	/** @brief True when DLSS-G reports vertical sync is usable while it is generating (SL-VSYNC-011). */
	[[nodiscard]] bool IsDLSSGVsyncSupported() const;
	[[nodiscard]] bool IsDLSSGDynamicSupported() const;

	/** @brief Sets the desired DLSS-G runtime load state. */
	void SetDLSSGDesiredLoaded(bool a_loaded);
	[[nodiscard]] bool IsDLSSGLoaded() const;
	[[nodiscard]] bool IsDLSSGLoadSettled() const;

	/** @brief Sets the desired FSR frame-generation runtime load state. */
	void SetFSRFGDesiredLoaded(bool a_loaded);
	[[nodiscard]] bool IsFSRFGLoaded() const;
	[[nodiscard]] bool IsFSRFGLoadSettled() const;
	[[nodiscard]] bool IsFSRFGPresentOwner() const;

	/**
	 * @brief Tags DLSS-G's inputs for the frame: the UI stage's inputs pass in graph modes (a_depthCopy names kMAIN_COPY's depth),
	 * the interop ring in plain D3D11 mode.
	 */
	void TagDLSSGResources(ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
		ID3D11Resource* a_hudlessColor, uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_displayWidth, uint32_t a_displayHeight, float a_jitterX, float a_jitterY, bool a_depthCopy);

	void ClearDLSSGTags();
	[[nodiscard]] bool EnsureDLSSGPresentTag();

	/**
	 * @brief Native mode: turns DLSS-G's mode off before the swap chain is replaced or resized, as Streamline asks. The controller
	 * turns it on again once the new swap chain is adopted. Render thread.
	 */
	void TurnDLSSGOffBeforeSwapchainChange();
	/** @brief FSR-FG counterpart: passes frames through when the render pass prepared none. */
	bool EnsureFSRFGPresentState();

	/**
	 * @brief The presenter's torn-down step, between the swap chain's destruction and creation: FFX's swap chain teardown and
	 * the frame generation features' load changes, which Streamline allows only while no swap chain exists.
	 */
	static bool CompleteSwapchainTeardown();

private:
	Streamline() = default;

	bool triedInit = false;
	bool initialized = false;
	bool native = false;  // initialized for the native device (InitializeForNative): manual hooking, evaluates as external work
	std::vector<uint32_t> loadedFeatures;
	std::wstring pluginDirectory;
	bool vulkanDeviceSet = false;
	// Set when Streamline genuinely cannot be brought up (no plugin dir, missing sl.interposer.dll,
	// unresolvable exports, slInit failure). Never a configuration choice: the interposer is always
	// preloaded so every upscaler and frame generation can be switched on in-game without a restart.
	bool unavailable = false;

	bool featureDLSS = false;
	bool featureReflex = false;
	bool featureDLSSG = false;
	bool featureXeSS = false;
	bool featureFSR = false;
	bool featureFSRFG = false;

	// DLSS-G requires VK_NV_optical_flow before Streamline initialization.
	bool dlssgHardware = false;
	// Native mode: the adapter the device is made on (InitializeForNative), which the DLSS-G hardware probe checks.
	uint64_t nativeAdapterLuid = 0;

	bool isNvidiaGPU = false;
	bool isIntelGPU = false;
	bool isRTXBelow40Series = false;
};

#pragma once

#include "Feature.h"
#include "Features/Screenshot/Burst.h"
#include "Features/Screenshot/ORGScreenshot.h"
#include "Utils/Subrect.h"
#include <DirectXTex.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class Texture2D;

struct ScreenshotFeature : public Feature
{
	/** @brief Stops the background screenshot worker thread on destruction. */
	virtual ~ScreenshotFeature();
	virtual std::string GetName() override { return "Screenshot"; }
	virtual std::string GetDisplayName() override { return T("feature.screenshot.name", "Screenshot"); }
	virtual std::string GetShortName() override { return "Screenshot"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kUtility; }

	/** @brief Returns true, indicating this feature's settings are always visible in the menu. */
	virtual bool IsInMenu() const override;

	/** @brief Draws the ImGui settings UI for screenshot path, format, crop, and hotkey configuration. */
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& a_json) override;
	virtual void SaveSettings(json& a_json) override;
	/** @brief Resets transient state (no-op for this feature). */
	virtual void Reset() override;
	/** @brief Called after all features are loaded: the vanilla screenshot setting, and the render graph's Capture anchor. */
	virtual void PostPostLoad() override;
	/** @brief Registers the render graph's capture copy in graph mode (ORGScreenshot). */
	virtual void SetupResources() override;

	/** @brief Captures a screenshot from the current back buffer and enqueues it for async encoding and save. */
	void Capture();
	/** @brief Hands finished GPU readbacks to the worker, then executes Capture() if one is pending. Render thread only; call before the wrapped buffers are cleared. */
	void ProcessCaptureRequest();
	/** @brief Copies the scene before the HUD draws when an SDR capture is pending, so screenshots exclude the HUD and menu blur. */
	void SnapshotCleanScene();
	/**
	 * @brief In graph modes, at the end of the ImageSpace chain: a pending SDR capture's clean scene is copied on the render graph
	 * (ORGScreenshot), which needs no D3D11 content. SnapshotCleanScene then copies only for the crop preview.
	 */
	void SnapshotCleanSceneOnGraph();
	/** @brief Returns this frame's pre-UI D3D11 snapshot, or nullptr if none was taken. */
	ID3D11Texture2D* GetFreshCleanScene() const;
	/** @brief Whether this frame has a pre-UI snapshot, the D3D11 one or the render graph's. */
	bool HasFreshCleanScene() const;
	/**
	 * @brief Drops a queued vanilla Multi-Screenshot (Ctrl+PrintScreen) while vanilla screenshots are disabled.
	 * Its TestCameraPath mode locks game time to 33 ms per frame, speeding the game up. Call after input dispatch.
	 */
	void SuppressVanillaMultiScreenshot() const;
	/**
	 * @brief Any thread: the back buffer of the next Present whose frame renders snapshot a_minSnapshot or a newer one (0: the next
	 * Present) saved to a_outputPath (its extension replaced by the format's), for scripted runs that must tie a capture to a frame
	 * (DCLF's test harness, dclf:shot=, cs-test's shots). Later requests wait behind it.
	 */
	void RequestCapture(std::filesystem::path a_outputPath, std::uint64_t a_minSnapshot = 0);
	/**
	 * @brief Any thread: a burst (ScreenshotBurst) of a_count captures of consecutive rendered frames, from the first Present whose
	 * frame renders a_minSnapshot or newer, into a_directory. It waits behind earlier requests and counts as one in
	 * ScriptedCapturesPending until its files are written.
	 */
	void RequestBurst(std::filesystem::path a_directory, std::uint32_t a_count, bool a_png, std::uint64_t a_minSnapshot);
	/** @brief Any thread: whether a RequestCapture capture is still to be saved (requested, read back or being encoded). */
	bool ScriptedCapturesPending() const { return scriptedInFlight.load(std::memory_order_acquire) != 0; }
	bool applyCropToScreenshot = true;

	// Settings
	std::string screenshotPath = "Screenshots";
	// HDR PNG quantization (7-16); used when HDR Display captures the back buffer.
	unsigned int hdrPngBitDepth = 11;
	bool hdrUseAvif = false;
	// Strip cICP from HDR PNGs so Discord falls back to the embedded ICC profile.
	bool fixDiscordHdr = true;
	// SDR output.
	bool sdrUsePng = true;
	// After save, put the file path on the clipboard (CF_HDROP).
	bool copyToClipboard = true;
	// Suppress the game's own PrintScreen capture by clearing bAllowScreenShot in memory, and its Multi-Screenshot.
	bool disableVanillaScreenshot = true;

	std::atomic<bool> captureRequested{ false };
	// RequestCapture's captures not yet saved (or failed): the test runner's exit waits for them.
	std::atomic<std::uint32_t> scriptedInFlight{ 0 };

private:
	struct PendingScreenshot
	{
		winrt::com_ptr<ID3D11Texture2D> stagingTexture;
		DirectX::ScratchImage image;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint32_t width = 0;
		uint32_t height = 0;
		std::filesystem::path outputPath;
		bool saveAsHdrPng = false;
		bool saveAsHdrAvif = false;
		bool saveAsSdrPng = false;
		int hdrPngBitDepth = 11;
		bool fixDiscordHdr = true;
		bool copyToClipboard = true;
		// Suppress the game's own PrintScreen capture by clearing bAllowScreenShot in memory.
		bool disableVanillaScreenshot = true;
		bool notify = true;  // the in-game "saved" message (not for RequestCapture's)
		// A render-graph readback (ORGScreenshot) of the whole source, which the worker crops at cropX, cropY.
		ORGScreenshot::Pixels readback;
		uint32_t cropX = 0;
		uint32_t cropY = 0;
		// A burst's frame (saved as a PNG only when the burst asks), or with burstEnd its end: the worker writes its files.
		std::shared_ptr<ScreenshotBurst::Burst> burst;
		ScreenshotBurst::Frame burstFrame;
		bool burstEnd = false;
	};

	std::mutex screenshotQueueMutex;
	std::condition_variable screenshotQueueCV;
	std::queue<PendingScreenshot> screenshotQueue;
	std::thread screenshotWorker;
	bool screenshotWorkerRunning = false;
	// Render thread only: the worker never touches the immediate context.
	std::vector<PendingScreenshot> pendingReadbacks;
	Util::Subrect::Controller subrect;
	// bAllowScreenShot as loaded from Skyrim.ini, captured before we first override it.
	std::optional<bool> vanillaScreenshotDefault;
	// RequestCapture's paths and the snapshot each must render, taken in order by the next Presents (one a Present).
	struct RequestedCapture
	{
		std::filesystem::path path;
		std::uint64_t minSnapshot = 0;
		std::shared_ptr<ScreenshotBurst::Burst> burst;  // RequestBurst's
	};
	std::mutex requestedPathsMutex;
	std::queue<RequestedCapture> requestedPaths;
	std::filesystem::path captureOutputPath;  // render thread: the path Capture() saves to (empty: the user's capture, BuildScreenshotPath)

	// SRV-readable copy used when the capture source's own SRV can't be sampled
	// directly (kFRAMEBUFFER on flat aliases the swap-chain backbuffer).
	winrt::com_ptr<ID3D11Texture2D> previewCacheTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> previewCacheSRV;
	std::unique_ptr<Texture2D> cleanSceneTexture;
	uint32_t cleanSceneFrame = UINT32_MAX;
	// Render thread only: a request already waited one frame for its pre-UI snapshot.
	bool captureDeferred = false;
	// Render thread only: set while the crop preview is drawn so the next frame snapshots the clean scene.
	bool previewVisible = false;
	// Render thread only: the burst taking captures, and the last Presents' times (for its frame time before it started).
	std::shared_ptr<ScreenshotBurst::Burst> activeBurst;
	std::array<double, 32> presentMs{};
	std::uint32_t presentsTimed = 0;
	std::uint32_t lastTimedPresent = ~0u;

	/** @brief Capture()'s work. For a burst's frame it saves into the burst and returns false, with a_why, when the frame cannot be taken. */
	bool CaptureInto(const std::shared_ptr<ScreenshotBurst::Burst>& a_burst, std::string* a_why);
	/** @brief Once a Present: the active burst's next capture, its start, or its end. */
	void StepBurst();
	/** @brief The worker's part of a burst: a frame's grid (and PNG), or the files at its end. */
	void ProcessBurstEntry(PendingScreenshot& a_screenshot);

	/** @brief Polls in-flight staging copies without stalling and enqueues the finished ones for encoding. */
	void ProcessPendingReadbacks();
	/** @brief Writes bAllowScreenShot:Display in memory from disableVanillaScreenshot, restoring the ini value when off. */
	void ApplyVanillaScreenshotSetting();
	void EnsureWorkerThread();
	void StopWorkerThread();
	void EnqueueScreenshot(PendingScreenshot&& screenshot);
	void ScreenshotWorkerLoop();
	void EnsurePreviewCache(ID3D11Texture2D* sourceTexture);
	static void ShowInGameNotification(std::string message);
};

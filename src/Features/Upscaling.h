#pragma once

#include "Feature.h"
#include "Utils/Setting.h"
#include <array>
#include <atomic>
#include <d3d11_4.h>
#include <winrt/base.h>

/** @brief Provides Vulkan-backed temporal upscaling and frame generation. */
struct Upscaling : Feature
{
private:
	static constexpr std::string_view MOD_ID = "156952";

public:
	/** @brief Binds the settings that raise frame revision events. */
	Upscaling();

	// Feature interface
	virtual inline std::string GetName() override { return "Upscaling"; }
	virtual std::string GetDisplayName() override { return T("feature.upscaling.name", "Upscaling"); }
	virtual inline std::string GetShortName() override { return "Upscaling"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline bool IsCore() const override { return false; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.upscaling.description", "Advanced upscaling technologies for improved performance"),
			{ T("feature.upscaling.key_feature_1", "DLSS (Deep Learning Super Sampling) support"),
				T("feature.upscaling.key_feature_2", "FSR (FidelityFX Super Resolution) support"),
				T("feature.upscaling.key_feature_5", "XeSS (Xe Super Sampling) support"),
				T("feature.upscaling.key_feature_3", "TAA (Temporal Anti-Aliasing) support"),
				T("feature.upscaling.key_feature_4", "FSR 3 and DLSS frame generation support") } };
	};

	float2 jitter = { 0, 0 };

	enum class UpscaleMethod
	{
		kNONE,
		kTAA,
		kFSR,
		kDLSS,
		kXeSS,
	};

	enum class FrameGenMethod
	{
		kFSR,
		kDLSSG,
	};

	struct Settings
	{
		// The method, the quality and frame generation choose textures, clients and the render size: frame revision events.
		CS::Setting<uint> upscaleMethod = (uint)UpscaleMethod::kFSR;
		CS::Setting<uint> upscaleMethodNoDLSS = (uint)UpscaleMethod::kFSR;
		CS::Setting<uint> qualityMode = 1;
		float sharpnessFSR = 0.0f;
		bool reflexEnabled = false;
		bool reflexBoost = false;
		// Legacy fields kept for JSON backward compatibility.
		bool reflexLowLatencyMode = false;
		bool reflexLowLatencyBoost = false;
		CS::Setting<bool> frameGeneration = false;
		CS::Setting<uint> frameGenMethod = (uint)FrameGenMethod::kDLSSG;
		CS::Setting<uint> frameGenMultiplier = 2;
		bool dlssgDynamic = false;
		bool fgShowOnlyGenerated = false;
		bool fgDebugView = false;
		bool fgDebugTearLines = false;
		bool fgDebugPacingLines = false;
		bool hardwareDefaultsApplied = false;

		bool vsync = false;
		// Zero disables the cap; positive values divide the monitor refresh rate.
		int frameRateLimitDivisor = 0;
	};

	Settings settings;

	struct JitterCB
	{
		float2 jitter;
		float useWideKernel;
		float pad0;
	};

	ConstantBuffer* jitterCB = nullptr;

	// Runtime state
	bool isWindowed = false;

	/** @brief Returns whether the game window is minimized. */
	static bool IsWindowMinimized();

	static void NotifyWindowFocus(bool a_focused);        // WM_ACTIVATEAPP / WM_ACTIVATE
	/**
	 * @brief Any thread (a test run's dclf:fg command): frame generation set as the menu's method stepper sets it. The render
	 * thread writes the settings at the next render frame's start, and a frame revision carries them. 0: off, 1: FSR-FG, 2: DLSS-G.
	 */
	static void RequestFrameGenTest(int a_method);
	/** @brief A test run's upscale method (the "upscale" command: an UpscaleMethod), written as RequestFrameGenTest writes its settings. */
	static void RequestUpscaleTest(int a_method);
	static void NotifyWindowModifying(bool a_modifying);  // WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE
	static void NotifyDisplayState(bool a_on);            // GUID_CONSOLE_DISPLAY_STATE
	static void NotifyDesktopChanged();                   // WM_DISPLAYCHANGE

	/** @brief With CS_FG_DISPLAY_SUSPEND=1, whether the display is off or the desktop changed moments ago: frame generation stays down until it settles. */
	static bool IsDisplayUnsettled();

	/**
	* @brief Returns whether presenting must be suspended. Once frames begin, this is the answer LatchWindowState took at the
	* current frame's Begin, so every reader in one frame agrees. Before that it reads the window now.
	*/
	static bool IsWindowUnusable();

	/**
	* @brief Reads the window state for the frame that begins now (Renderer::Begin). The frame's description and its upscaler
	* read the same answer, so a focus change mid-frame takes effect at the next Begin.
	*/
	static void LatchWindowState();

	static inline std::atomic<bool> s_windowUnfocused{ false };
	static inline std::atomic<int> s_frameGenTestRequest{ -1 };  // RequestFrameGenTest's, -1: none
	static inline std::atomic<int> s_upscaleTestRequest{ -1 };   // RequestUpscaleTest's, -1: none
	static inline std::atomic<bool> s_windowModifying{ false };
	static inline std::atomic<bool> s_windowUnusableLatched{ false };
	static inline std::atomic<bool> s_windowLatched{ false };  // LatchWindowState has run
	static inline std::atomic<bool> s_displayOff{ false };
	static inline std::atomic<std::uint64_t> s_displayChangedTick{ 0 };

	// Timing and scaling
	double refreshRate = 0.0;
	float2 resolutionScale = { 1.0f, 1.0f };

	bool IsUpscalingActive() const;

	// Feature interface overrides
	virtual void DrawSettings() override;
	virtual void SaveSettings(json& o_json) override;
	virtual void LoadSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DataLoaded() override;

	virtual void Load() override;
	virtual void PostPostLoad() override;
	virtual void SetupResources() override;

	/** @brief The method the settings choose, after falling back for missing SDK support. What the next frame revision carries. */
	UpscaleMethod GetUpscaleMethod() const;
	FrameGenMethod GetFrameGenMethod() const;

	/**
	 * @brief Render thread. The method this frame runs: the adopted revision's with the render graph, so it switches in with the
	 * textures and builds made for it. GetUpscaleMethod in plain D3D11 mode.
	 */
	UpscaleMethod FrameUpscaleMethod() const;
	/** @brief Render thread. The quality mode this frame runs, chosen as FrameUpscaleMethod is. */
	uint FrameQualityMode() const;

	void ApplyHardwareDefaults();

	winrt::com_ptr<ID3D11PixelShader> depthRefractionUpscalePS;
	ID3D11PixelShader* GetDepthRefractionUpscalePS();

	winrt::com_ptr<ID3D11PixelShader> underwaterMaskUpscalePS;
	ID3D11PixelShader* GetUnderwaterMaskUpscalePS();

	winrt::com_ptr<ID3D11VertexShader> upscaleVS;
	ID3D11VertexShader* GetUpscaleVS();

	winrt::com_ptr<ID3D11PixelShader> copyHudlessPS;
	ID3D11PixelShader* GetCopyHudlessPS();

	winrt::com_ptr<ID3D11DepthStencilState> upscaleDepthStencilState;
	winrt::com_ptr<ID3D11BlendState> upscaleBlendState;
	winrt::com_ptr<ID3D11RasterizerState> upscaleRasterizerState;

	void ConfigureTAA();
	void ConfigureUpscaling(RE::BSGraphics::State* a_state);
	void Upscale();
	/**
	 * @brief Before an evaluate: DXVK work between the last render-graph epoch and the upscaler. The rule and its evidence:
	 * docs/development/render-graph.md, "Hazard: external Vulkan work directly after a graph epoch". Frame generation is not covered.
	 */
	void SeparateFromGraphEpochs();

	/** @brief Frame generation is selected and its presenter's surface is ready for this frame's inputs. */
	bool IsFrameGenerationActive() const;
	/** @brief Frame generation is selected and can run, whether or not a presentation replacement is still in flight. */
	bool IsFrameGenerationSelected() const;

	/** @brief Returns the Reflex state required by the active frame generator. */
	[[nodiscard]] bool GetEffectiveReflex() const;

	/** @brief Returns the monitor refresh rate in hertz. */
	/// Lowest frame-rate target the divisor list will offer. A divisor that cannot reach this at
	/// the current refresh rate is clamped in GetTargetFrameRate, so a setting saved against a
	/// faster display cannot silently cap the game.
	static constexpr int kMinTargetFps = 30;

	/// Divisors the settings stepper offers at a given refresh rate, "Unlocked" (0) last.
	static std::vector<int> FrameRateDivisorOptions(int a_refresh);
	/// The divisor actually in force: the saved one if the refresh rate can support it, else the
	/// fallback the stepper shows. Shared by the UI and GetTargetFrameRate so they cannot disagree.
	static int ResolveFrameRateDivisor(int a_saved, int a_refresh);

	[[nodiscard]] int GetMonitorRefreshRate() const;
	/** @brief Highest refresh rate the display offers at its current resolution. */
	[[nodiscard]] int GetHighestRefreshRate() const;
	/** @brief DXVK tearing preference implied by the frame-rate setting: 1 = tearing, 0 = tear-free. */
	[[nodiscard]] uint32_t GetPresentModePreference() const;
	/** @brief Re-applies the present mode when the frame-rate setting changes; recreates the swapchain. */
	void UpdatePresentModePreference();
	/** @brief Returns the configured frame-rate cap, or zero when uncapped. */
	[[nodiscard]] double GetTargetFrameRate() const;
	/** @brief Returns the rendered-frame cap after accounting for fixed frame generation. */
	[[nodiscard]] double GetRenderedFrameRateLimit() const;
	/** @brief Returns the fixed DLSS-G multiplier clamped to the reported hardware limit. */
	[[nodiscard]] uint32_t GetFixedDLSSGMultiplier() const;
	HRESULT PresentWithFrameGeneration(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags,
		const std::function<HRESULT(IDXGISwapChain*, UINT, UINT)>& a_present);

	// D3D11 textures
	Texture2D* upscaledTexture = nullptr;
	// This frame's hudless texture, one of hudlessTextures (ORGUpscaling::kHudlessFormats' order).
	Texture2D* hudlessTexture = nullptr;
	std::array<Texture2D*, 3> hudlessTextures{};
	// A small typed UAV DXVK clears before each evaluate (SeparateFromGraphEpochs).
	winrt::com_ptr<ID3D11UnorderedAccessView> epochSeparator;

	virtual void ClearShaderCache() override;

	float projectionPosScaleX = 0.0f;
	float projectionPosScaleY = 0.0f;

	float dynamicResolutionWidthRatio = 1.0f;
	float dynamicResolutionHeightRatio = 1.0f;

	bool depthUpscaleUseWideKernel = false;

	void PostDisplay();
	void PerformUpscaling();
	void UpscaleDepth();
	/** @brief The depth upscale's constants for this frame: the jitter, and the wide kernel's choice with its hysteresis. */
	JitterCB DepthUpscaleConstants();

	static double GetRefreshRate(HWND a_window);

private:
	static constexpr size_t kUpscaleMethodCount = static_cast<size_t>(UpscaleMethod::kXeSS) + 1;


	void BeginRenderFrame();
	/** @brief Plain D3D11: the upscaled texture at the display size, made once on first need and kept. The graph declares its own. */
	void CreateUpscaledTexture();
	/**
	 * @brief Plain D3D11: this frame's hudless texture, one per format, each made on first need and kept, so an HDR or presenter
	 * change selects another and destroys nothing. Null while the presenter has no usable format.
	 */
	Texture2D* SelectD3D11Hudless();
	ID3D11Resource* CaptureHudlessColor();
	/** @brief The upscaler's output as ORGUpscaling::Texture chooses it (the graph's export in graph mode); null when neither exists. */
	ID3D11Texture2D* UpscaledResource() const;
	/** @brief Frame generation's hudless colour, as UpscaledResource. */
	ID3D11Texture2D* HudlessResource() const;
	bool CopyHudlessColor(ID3D11ShaderResourceView* a_source);
	void PrepareFrameGeneration(ID3D11Resource* a_hudlessColor);

	struct Main_UpdateJitter
	{
		static void thunk(RE::BSGraphics::State* a_state);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_PostProcessing
	{
		static void thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_RenderPrecipitation
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSFaceGenManager_UpdatePendingCustomizationTextures
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
		static bool Register();
	};
};

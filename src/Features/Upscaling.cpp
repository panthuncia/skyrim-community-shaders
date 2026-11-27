#include "Upscaling.h"
#include "DrawcallLimitFix.h"
#include "RenderGraph/Frame/PostChain.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/FrameCamera.h"
#include "Snapshot/Publication.h"

#include "../I18n/I18n.h"
#include "Deferred.h"
#include "DxvkLoader.h"
#include "Features/Effects11/D3D11StateBackup.h"
#include "HDRDisplay.h"
#include "Hooks.h"
#include "ScreenshotFeature.h"
#include "State.h"
#include "Upscaling/DXVKInterop.h"
#include "Upscaling/DxvkPresenter.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"
#include "Upscaling/FrameGenController.h"
#include "Upscaling/ORGUpscaling.h"
#include "Upscaling/PresentationRevision.h"
#include "Upscaling/Streamline.h"
#include "Utils/Game.h"
#include "Utils/UI.h"
#include "Utils/ImportCallSites.h"
#include "Utils/VersionedRelocation.h"
#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <nlohmann/json.hpp>

#include "Features/PostProcessing.h"

#define I18N_KEY_PREFIX "feature.upscaling."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Upscaling::Settings,
	upscaleMethod,
	upscaleMethodNoDLSS,
	qualityMode,
	sharpnessFSR,
	reflexEnabled,
	reflexBoost,
	reflexLowLatencyMode,
	reflexLowLatencyBoost,
	frameGeneration,
	frameGenMethod,
	frameGenMultiplier,
	dlssgDynamic,
	fgShowOnlyGenerated,
	fgDebugView,
	fgDebugTearLines,
	fgDebugPacingLines,
	hardwareDefaultsApplied,
	vsync,
	frameRateLimitDivisor);

decltype(&D3D11CreateDeviceAndSwapChain) ptrD3D11CreateDeviceAndSwapChainUpscaling;

HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChainUpscaling(
	IDXGIAdapter* pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	[[maybe_unused]] const D3D_FEATURE_LEVEL* pFeatureLevels,
	[[maybe_unused]] UINT FeatureLevels,
	UINT SDKVersion,
	DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
	IDXGISwapChain** ppSwapChain,
	ID3D11Device** ppDevice,
	D3D_FEATURE_LEVEL* pFeatureLevel,
	ID3D11DeviceContext** ppImmediateContext)
{
	DXGI_ADAPTER_DESC adapterDesc;
	pAdapter->GetDesc(&adapterDesc);
	globals::state->SetAdapterDescription(adapterDesc.Description);

	auto& upscaling = globals::features::upscaling;

	// FLIP_DISCARD requires BufferCount >= 2 and a flip-model-compatible (non-sRGB) format.
	pSwapChainDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	if (pSwapChainDesc->BufferCount < 2)
		pSwapChainDesc->BufferCount = 2;

	if (globals::features::hdrDisplay.loaded) {
		logger::info("[Upscaling] Upgrading swap chain format from {} to R10G10B10A2_UNORM for HDR", static_cast<int>(pSwapChainDesc->BufferDesc.Format));
		pSwapChainDesc->BufferDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
	} else if (pSwapChainDesc->BufferDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
		pSwapChainDesc->BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	} else if (pSwapChainDesc->BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
		pSwapChainDesc->BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	}

	auto refreshRate = Upscaling::GetRefreshRate(pSwapChainDesc->OutputWindow);
	upscaling.refreshRate = refreshRate;

	upscaling.isWindowed = pSwapChainDesc->Windowed;

	const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;

	// CS_ORG_NATIVE=1: no D3D11 device. Hooks.cpp leaves device creation to this hook while Upscaling is loaded, so native mode's
	// device and swap chain are made here as they are there.
	if (RenderGraph::Engine::Native::Requested()) {
		if (globals::features::hdrDisplay.loaded)
			HDRDisplay::wasExclusiveFullscreen = !pSwapChainDesc->Windowed;
		return RenderGraph::Engine::Native::CreateDeviceAndSwapChain(pAdapter, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
	}

	const HRESULT hr = ptrD3D11CreateDeviceAndSwapChainUpscaling(pAdapter,
		DriverType,
		Software,
		Flags,
		&featureLevel,
		1,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		pFeatureLevel,
		ppImmediateContext);
	return hr;
}

Upscaling::Upscaling()
{
	settings.upscaleMethod.Bind(CS::SettingRegistry::Declare("Upscaling.upscaleMethod"));
	settings.upscaleMethodNoDLSS.Bind(CS::SettingRegistry::Declare("Upscaling.upscaleMethodNoDLSS"));
	settings.qualityMode.Bind(CS::SettingRegistry::Declare("Upscaling.qualityMode"));
	settings.frameGeneration.Bind(CS::SettingRegistry::Declare("Upscaling.frameGeneration"));
	settings.frameGenMethod.Bind(CS::SettingRegistry::Declare("Upscaling.frameGenMethod"));
	settings.frameGenMultiplier.Bind(CS::SettingRegistry::Declare("Upscaling.frameGenMultiplier"));
}

namespace
{
	/** @brief Whether the frame takes its upscaling values from the adopted frame revision: the render graph runs revisions. */
	bool FollowsRevisions()
	{
		return RenderGraph::Frame::Driver::Installed() && RenderGraphRuntime::Get().IsActive();
	}
}

static bool DrawStepper(const char* a_label, int* a_index, const std::vector<const char*>& a_options, bool a_disabled = false)
{
	const int count = static_cast<int>(a_options.size());
	if (count == 0)
		return false;
	*a_index = std::clamp(*a_index, 0, count - 1);

	ImGui::BeginDisabled(a_disabled);
	const bool changed = ImGui::SliderInt(a_label, a_index, 0, count - 1, a_options[*a_index], ImGuiSliderFlags_NoInput);
	*a_index = std::clamp(*a_index, 0, count - 1);
	ImGui::EndDisabled();
	return changed;
}

static bool DrawToggleStepper(const char* a_label, bool* a_value, bool a_disabled = false)
{
	const std::vector<const char*> onOff = { "Off", "On" };
	int idx = *a_value ? 1 : 0;
	if (DrawStepper(a_label, &idx, onOff, a_disabled)) {
		*a_value = (idx == 1);
		return true;
	}
	return false;
}

void Upscaling::DrawSettings()
{
	auto* streamline = Streamline::GetSingleton();
	const bool dlssAvailable = streamline->IsDLSSSupported();
	const bool xessAvailable = streamline->IsXeSSSupported();

	const bool dlssgAvailable = streamline->IsDLSSGSupported();
	const bool fsrfgAvailable = streamline->IsFSRFGSupported();
	const bool reflexAvailable = streamline->IsReflexSupported();

	auto selectUpscaler = [&](UpscaleMethod a_m) {
		settings.upscaleMethod = (uint)a_m;
		if (a_m != UpscaleMethod::kDLSS)
			settings.upscaleMethodNoDLSS = (uint)a_m;
	};

	ImGui::SeparatorText(T(TKEY("display_header"), "Display"));
	{
		// SL-VSYNC-011, as the Streamline sample does it: DLSS-G reports whether vsync is usable
		// while generating, and the toggle is disabled and shown as off only when it says no --
		// rather than assuming DLSS-G always precludes it.
		const bool vsyncForcedOff = IsFrameGenerationActive() && GetFrameGenMethod() == FrameGenMethod::kDLSSG &&
		                            !Streamline::GetSingleton()->IsDLSSGVsyncSupported();
		if (vsyncForcedOff) {
			bool effectiveVsync = false;
			DrawToggleStepper(T(TKEY("vsync"), "Vertical Synchronisation"), &effectiveVsync, /*disabled=*/true);
			ImGui::SameLine();
			ImGui::TextDisabled("%s", T(TKEY("vsync_dlssg"), "(forced off by DLSS-G)"));
		} else {
			DrawToggleStepper(T(TKEY("vsync"), "Vertical Synchronisation"), &settings.vsync);
		}

		const int refresh = GetMonitorRefreshRate();
		const std::vector<int> divisorOptions = FrameRateDivisorOptions(refresh);

		std::vector<std::string> fpsStrings;
		for (int d : divisorOptions)
			fpsStrings.push_back(d == 0 ?
			                         std::string(T(TKEY("frame_rate_unlocked"), "Unlocked (variable)")) :
			                         std::format("{} FPS", refresh / d));
		std::vector<const char*> fpsLabels;
		for (auto& s : fpsStrings)
			fpsLabels.push_back(s.c_str());

		const int maxSel = static_cast<int>(divisorOptions.size()) - 1;
		const int effective = ResolveFrameRateDivisor(settings.frameRateLimitDivisor, refresh);
		int sel = 0;
		for (int i = 0; i <= maxSel; ++i) {
			if (divisorOptions[i] == effective)
				sel = i;
		}
		DrawStepper(T(TKEY("frame_rate"), "Frame Rate"), &sel, fpsLabels);
		settings.frameRateLimitDivisor = divisorOptions[std::clamp(sel, 0, maxSel)];
	}

	ImGui::SeparatorText(T(TKEY("upscaling_header"), "Upscaling"));
	{
		std::vector<const char*>   techLabels = { "Off", T(TKEY("method_taa"), "TAA"), "FSR" };
		std::vector<UpscaleMethod> techMethods = { UpscaleMethod::kNONE, UpscaleMethod::kTAA, UpscaleMethod::kFSR };
		if (xessAvailable) {
			techLabels.push_back("XeSS");
			techMethods.push_back(UpscaleMethod::kXeSS);
		}
		if (dlssAvailable) {
			techLabels.push_back("DLSS");
			techMethods.push_back(UpscaleMethod::kDLSS);
		}

		int techIdx = 0;
		for (int i = 0; i < static_cast<int>(techMethods.size()); ++i)
			if (settings.upscaleMethod == (uint)techMethods[i])
				techIdx = i;
		if (DrawStepper(T(TKEY("upscaling_technique"), "Upscaling Technique"), &techIdx, techLabels))
			selectUpscaler(techMethods[std::clamp(techIdx, 0, static_cast<int>(techMethods.size()) - 1)]);

		if (streamline->IsUnavailable())
			ImGui::TextDisabled("%s", T(TKEY("sl_unavailable_note"), "Streamline could not be loaded - upscalers and frame generation are unavailable"));

		const UpscaleMethod cur = (UpscaleMethod)settings.upscaleMethod.Get();
		const bool hasPreset = (cur == UpscaleMethod::kFSR || cur == UpscaleMethod::kXeSS || cur == UpscaleMethod::kDLSS);
		if (hasPreset) {
			const std::vector<const char*> presets = {
				T(TKEY("preset_native"), "Native"),
				T(TKEY("preset_quality"), "Quality"),
				T(TKEY("preset_balanced"), "Balanced"),
				T(TKEY("preset_performance"), "Performance"),
				T(TKEY("preset_ultra_performance"), "Ultra Performance"),
			};
			int pIdx = std::clamp((int)settings.qualityMode, 0, 4);
			if (DrawStepper(T(TKEY("upscale_preset"), "Upscale Preset"), &pIdx, presets))
				settings.qualityMode = (uint)std::clamp(pIdx, 0, 4);

			if (cur == UpscaleMethod::kFSR)
				ImGui::SliderFloat(T(TKEY("sharpness"), "Sharpness"), &settings.sharpnessFSR, 0.0f, 1.0f, "%.1f");

			// XeSS reaches its advertised quality only on Intel Arc, where it runs on XMX units.
			// On any other GPU it silently uses the DP4a fallback, which resolves noticeably softer
			// than FSR at the same preset. Users reasonably read that as the upscaler being broken.
			if (cur == UpscaleMethod::kXeSS && !streamline->IsXeSSHardwareAccelerated()) {
				ImGui::TextWrapped("%s", T(TKEY("xess_dp4a_note"),
					"XeSS is running its DP4a fallback: hardware acceleration needs an Intel Arc GPU. "
					"Expect a softer image than FSR at the same preset."));
			}
		}
	}

	ImGui::SeparatorText(T(TKEY("fg_header"), "Frame Generation"));
	{
		std::vector<const char*>    fgLabels = { T(TKEY("fg_method_none"), "None") };
		std::vector<FrameGenMethod> fgMethods = { FrameGenMethod::kFSR };
		if (fsrfgAvailable) {
			fgLabels.push_back(T(TKEY("fg_method_fsr"), "FSR FG"));
			fgMethods.push_back(FrameGenMethod::kFSR);
		}
		if (dlssgAvailable) {
			fgLabels.push_back(T(TKEY("fg_method_dlssg"), "DLSS FG"));
			fgMethods.push_back(FrameGenMethod::kDLSSG);
		}

		int fgSel = 0;
		if (settings.frameGeneration) {
			const FrameGenMethod active = GetFrameGenMethod();
			for (int i = 1; i < static_cast<int>(fgMethods.size()); ++i)
				if (fgMethods[i] == active)
					fgSel = i;
		}
		if (DrawStepper(T(TKEY("fg_method"), "Frame Generation Method"), &fgSel, fgLabels)) {
			settings.frameGeneration = (fgSel != 0);
			if (fgSel != 0)
				settings.frameGenMethod = (uint)fgMethods[std::clamp(fgSel, 0, static_cast<int>(fgMethods.size()) - 1)];
		}

		if (settings.frameGeneration && GetFrameGenMethod() == FrameGenMethod::kDLSSG && dlssgAvailable) {
			const uint32_t maxFrames = streamline->GetDLSSGMaxFramesToGenerate();
			const uint     maxMultiplier = std::clamp<uint>(maxFrames > 0u ? maxFrames + 1u : 2u, 2u, 6u);
			std::vector<std::string> multStrings = {
				streamline->IsDLSSGDynamicSupported()
					? std::string(T(TKEY("fg_dynamic"), "Dynamic"))
					: std::string(T(TKEY("fg_auto"), "Auto")) };
			for (uint m = 2; m <= maxMultiplier; ++m)
				multStrings.push_back(std::format("{}x", m));
			std::vector<const char*> multStates;
			for (auto& s : multStrings)
				multStates.push_back(s.c_str());

			int multIdx = settings.dlssgDynamic ? 0 :
			              std::clamp((int)settings.frameGenMultiplier - 1, 1, (int)maxMultiplier - 1);
			if (DrawStepper(T(TKEY("fg_multiplier"), "Frame Generation Multiplier"), &multIdx, multStates)) {
				settings.dlssgDynamic = (multIdx == 0);
				if (multIdx >= 1)
					settings.frameGenMultiplier = (uint)(multIdx + 1);
			}
		}
	}

	if (reflexAvailable) {
		ImGui::SeparatorText(T(TKEY("ll_header"), "Low Latency"));
		const std::vector<const char*> reflexStates = { "Off", "On", T(TKEY("reflex_on_boost"), "On + Boost") };
		const bool fgForcesReflex = IsFrameGenerationActive() &&
		                            (GetFrameGenMethod() == FrameGenMethod::kDLSSG || GetFrameGenMethod() == FrameGenMethod::kFSR);
		if (fgForcesReflex) {
			int idx = GetEffectiveReflex() ? (settings.reflexBoost ? 2 : 1) : 0;
			DrawStepper(T(TKEY("nv_reflex"), "NVIDIA Reflex Low Latency"), &idx, reflexStates, /*disabled=*/true);
			ImGui::SameLine();
			ImGui::TextDisabled("%s", GetFrameGenMethod() == FrameGenMethod::kDLSSG ?
			                              T(TKEY("reflex_forced_dlssg"), "(forced on by DLSS-G)") :
			                              T(TKEY("reflex_forced_fsrfg"), "(forced off by FSR frame gen)"));
		} else {
			int idx = settings.reflexEnabled ? (settings.reflexBoost ? 2 : 1) : 0;
			if (DrawStepper(T(TKEY("nv_reflex"), "NVIDIA Reflex Low Latency"), &idx, reflexStates)) {
				settings.reflexEnabled = (idx >= 1);
				settings.reflexBoost = (idx == 2);
			}
		}
	}
}

void Upscaling::SaveSettings(json& o_json)
{
	o_json = settings;
	auto iniSettingCollection = CS_ENGINE_READ(Ini, Other, globals::game::iniPrefSettingCollection);
	if (iniSettingCollection) {
		auto setting = iniSettingCollection->GetSetting("bUseTAA:Display");
		if (setting) {
			iniSettingCollection->WriteSetting(setting);
		}
	}
}

void Upscaling::LoadSettings(json& o_json)
{
	settings = o_json;

	constexpr auto enumCount = 5;
	if (settings.upscaleMethod >= static_cast<uint>(enumCount)) {
		logger::warn("[Upscaling] Loaded upscaleMethod {} out of range, clamping to {}", settings.upscaleMethod.Get(), enumCount - 1);
		settings.upscaleMethod = enumCount - 1;
	}
	if (settings.upscaleMethodNoDLSS >= static_cast<uint>(enumCount) ||
		settings.upscaleMethodNoDLSS == static_cast<uint>(UpscaleMethod::kDLSS))
		settings.upscaleMethodNoDLSS = static_cast<uint>(UpscaleMethod::kFSR);

	constexpr auto fgMethodCount = 2;
	if (settings.frameGenMethod >= static_cast<uint>(fgMethodCount))
		settings.frameGenMethod = static_cast<uint>(FrameGenMethod::kDLSSG);

	if (settings.reflexLowLatencyMode && !settings.reflexEnabled) {
		settings.reflexEnabled = true;
		settings.reflexBoost = settings.reflexLowLatencyBoost;
		settings.reflexLowLatencyMode = false;
		settings.reflexLowLatencyBoost = false;
	}
	if (settings.qualityMode > 4) {
		logger::warn("[Upscaling] Loaded qualityMode {} out of range, clamping to 4 (Ultra Performance)", settings.qualityMode.Get());
		settings.qualityMode = 4;
	}
	auto iniSettingCollection = CS_ENGINE_READ(Ini, Other, globals::game::iniPrefSettingCollection);
	if (iniSettingCollection) {
		auto setting = iniSettingCollection->GetSetting("bUseTAA:Display");
		if (setting) {
			iniSettingCollection->ReadSetting(setting);
		}
	}
}

void Upscaling::RestoreDefaultSettings()
{
	settings = {};
}

void Upscaling::DataLoaded()
{
	Util::DisableVanillaTAA();

	static auto fDRClampOffset = CS_ENGINE_READ(Ini, Other, RE::GetINISetting("fDRClampOffset:Display"));
	fDRClampOffset->data.f = 0.0f;

	// unk88 keeps the dynamic-resolution viewport, which rounds the 16x1 lens flare visibility target to nothing
	const auto& effects = CS_ENGINE_READ(ImageSpace, Other, RE::ImageSpaceManager::GetSingleton())->effects;
	const auto visibilityIndex = RE::ImageSpaceManager::GetCurrentIndex(RE::ImageSpaceManager::ISLensFlareVisibility);
	if (visibilityIndex < effects.size() && effects[visibilityIndex])
		effects[visibilityIndex]->unk88 = false;
}

void Upscaling::Load()
{
	// Frame-generation proxies require synchronous present from the first frame.
	if (DxvkLoader::IsLoaded())
		FrameGen::DxvkPresenter::Get().PresetSyncPresent(settings.frameGeneration);

	if (DxvkLoader::IsLoaded()) {
		// Tear-free (MAILBOX) for the whole session, set before DXVK creates the swapchain.
		//
		// DLSS-G's flip metering cannot space its generated frame across tearing presents: with
		// IMMEDIATE the real and generated frames land back to back -- 49.6% of present intervals at
		// 0.0 ms and the rest near 48.5 ms instead of an even 24.2 ms -- which cost 5.1% of presents
		// to drops and left frame-time deviation at 16.5 ms. MAILBOX brings that to 2.3 ms and puts
		// output back on target.
		//
		// Correction, measured with PresentMon rather than the overlay: the 2.3 ms above is the
		// RENDER frame time, and it is not the cadence the display sees. Measuring 20 s of the
		// Whiterun bench directly, uncapped:
		//
		//                    present intervals        display changes
		//     FSR-FG         sd 0.39 ms,  0.0% <1ms   sd 0.51 ms, 0.0% <1ms
		//     DLSS-G         sd 3.39 ms, 49.7% <1ms   sd 1.02 ms, 1.1% <1ms
		//
		// The two columns disagree, and the second is the one that matters. sl.dlss_g emits its
		// generated frame from inside the same present as the real one (see the DLSS-G branch of
		// GetRenderedFrameRateLimit), so half its present CALLS land within a millisecond of each
		// other -- but DLSS-G on Ada meters flips in software, and by the time frames are scanned
		// out only 1.1% are still bunched. Reading MsBetweenPresents alone makes a working pacer
		// look broken.
		//
		// So DLSS-G is paced, about twice as loosely as FFX (1.02 ms against 0.51 ms on a 3.58 ms
		// mean) -- and that is the uncapped case at 279 fps, where the display cannot give each
		// frame its own scanout anyway. Capped, which is how the thing is actually played, its
		// pacing is exact: 595 display changes, mean 33.35 ms, sd 0.01 ms, every interval between
		// 33.31 and 33.40 ms.
		//
		// Chaining present IDs so it could use vkWaitForPresentKHR was tried and made both worse
		// -- display sd 1.11 ms, 2.1% bunched, 3% fewer frames -- so it is not waiting on them.
		// Choose the present mode on the merits below; none of the modes tested changed DLSS-G's
		// cadence either way.
		//
		// That reasoning holds only while the frame rate is capped. MAILBOX blocks in present at
		// vblank, which pins DLSS-G's output to the refresh rate and the rendered rate to
		// refresh/multiplier: measured 30 rendered / 60 presented with 25.5 ms of every frame spent
		// inside Present, against 144 / 288 and 0.8 ms on the same scene with a tearing-capable
		// mode. A user who asked for an unlimited frame rate is not asking for that, so the mode
		// follows the frame-rate setting: tear-free while a cap paces the output anyway, tearing
		// when the cap is off. Switching costs a swapchain recreate (MAILBOX is outside IMMEDIATE's
		// compatible-mode group on NVIDIA), so it is done only when the setting actually changes.
		FrameGen::DxvkPresenter::Get().PresetTearing(GetPresentModePreference() != 0);
	}

	if (DxvkLoader::IsLoaded()) {
		// Always preload, before DXVK creates VkInstance so its Vulkan loader aliases the interposer.
		// This deliberately does not consult saved settings. Interposition can only be established
		// here, so gating it on whichever upscaler/frame-generation values happened to be saved is
		// what forced a restart to turn either on. With the interposer always installed, every
		// upscaler and frame generation can be switched in-game. Nothing is spent until a feature is
		// actually evaluated -- DXVK still runs on the real driver in the meantime.
		Streamline::GetSingleton()->PreloadInterposer();
	}

	const auto iatOriginal = SKSE::PatchIAT(hk_D3D11CreateDeviceAndSwapChainUpscaling, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
	*(uintptr_t*)&ptrD3D11CreateDeviceAndSwapChainUpscaling = DxvkLoader::IsLoaded() ?
	                                                              reinterpret_cast<uintptr_t>(DxvkLoader::GetD3D11CreateDeviceAndSwapChain()) :
	                                                              iatOriginal;
	// The IAT patch alone does not survive RenderDoc; see RedirectImportCallSites.
	Util::RedirectImportCallSites(::GetModuleHandleW(nullptr), "d3d11.dll", "D3D11CreateDeviceAndSwapChain",
		reinterpret_cast<void*>(&hk_D3D11CreateDeviceAndSwapChainUpscaling));
}

struct BSImageSpace_Init_FXAA
{
	static void thunk()
	{
		func();

		// Force FXAA off after the game initializes its image-space state.
		auto fxaaEnabled = reinterpret_cast<bool*>(REL::RelocationID(513281, 391028).address());
		*fxaaEnabled = false;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct FaceGen_CopyRenderTarget
{
	static RE::NiTexture::RendererData* thunk(RE::BSGraphics::Renderer* a_renderer, RE::RENDER_TARGET a_target, D3D11_USAGE a_usage)
	{
		auto source = a_renderer->GetRuntimeData().renderTargets[a_target].texture;
		if (!source) {
			logger::error("[Upscaling] FaceGen texture copy skipped: missing source render target");
			return nullptr;
		}

		// CommonLib's GetSingleton() wrongly dereferences this global: the manager is stored inline, not behind a pointer
		static REL::Relocation<RE::BSGraphics::RenderTargetManager*> renderTargetManager{ REL::RelocationID(524970, 411451) };
		const auto& properties = renderTargetManager->renderTargetData[a_target];
		using CreateTexture = RE::NiTexture::RendererData*(RE::BSGraphics::Renderer*, uint32_t, uint32_t, const void*, D3D11_USAGE, DXGI_FORMAT, bool);
		static REL::Relocation<CreateTexture> createTexture{ REL::RelocationID(75511, 77303) };
		auto texture = createTexture(a_renderer, properties.width, properties.height, nullptr, a_usage, DXGI_FORMAT_R8G8B8A8_UNORM, false);
		if (!texture) {
			logger::error("[Upscaling] FaceGen texture allocation failed ({}x{}, usage {})", properties.width, properties.height, static_cast<uint32_t>(a_usage));
			return nullptr;
		}

		if (!texture->texture || (a_usage != D3D11_USAGE_STAGING && !texture->resourceView)) {
			logger::error("[Upscaling] FaceGen texture allocation returned incomplete resources ({}x{})", properties.width, properties.height);
			if (texture->resourceView)
				texture->resourceView->Release();
			if (texture->texture)
				texture->texture->Release();
			delete texture;
			return nullptr;
		}

		// The raw context takes D3D11 objects: the new texture may be an engine texture's tripwire handle (CS_ORG_ENGINE_TEXTURES).
		globals::d3d::context->CopyResource(RenderGraph::Engine::Tripwire::Unwrap(texture->texture), RenderGraph::Engine::Tripwire::Unwrap(source));
		return texture;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

void Upscaling::PostPostLoad()
{
	// The frame anchors run it after every late epoch: the upscaler, frame generation or the present may follow one directly.
	FrameAnchors::SetLateEpochHook([] { globals::features::upscaling.SeparateFromGraphEpochs(); });

	bool isGOG = !GetModuleHandle(L"steam_api64.dll");
	stl::detour_thunk<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084));

	// Calculates render resolution and temporal jitter.
	stl::write_thunk_call<Main_UpdateJitter>(REL::RelocationID(75460, 77245).address() + Util::VersionedRelocation::Select(0xE5, isGOG ? 0x133 : 0xE2, 0x133));

	// Disables the original dynamic-resolution system.
	REL::safe_write(REL::RelocationID(35556, 36555).address() + REL::Relocate(0x2D, 0x2D), REL::NOP5, sizeof(REL::NOP5));

	// Performs upscaling between volumetric lighting and post-processing.
	stl::write_thunk_call<Main_PostProcessing>(REL::RelocationID(100430, 107148).address() + REL::Relocate(0x1F0, 0x1E7));

	// Patches facegen texture generation to not use dynamic resolution
	const auto faceGenUpdate = REL::RelocationID(26455, 27041).address();
	const auto faceGenCopy = faceGenUpdate + Util::VersionedRelocation::Select(0x119, 0x187, 0x19E);
	if (Util::VersionedRelocation::IsCallTo(faceGenCopy, REL::RelocationID(75524, 77318).address())) {
		stl::write_thunk_call<FaceGen_CopyRenderTarget>(faceGenCopy);
	} else {
		logger::warn("[Upscaling] FaceGen texture allocation guard not installed: unexpected copy call");
	}
	stl::detour_thunk<BSFaceGenManager_UpdatePendingCustomizationTextures>(faceGenUpdate);

	// Prevents dynamic resolution from affecting the precipitation camera.
	stl::write_thunk_call<Main_RenderPrecipitation>(REL::RelocationID(35560, 36559).address() + Util::VersionedRelocation::Select(0x3A1, 0x3A1, 0x3BF));

	// Forces FXAA off.
	stl::detour_thunk<BSImageSpace_Init_FXAA>(REL::RelocationID(98974, 105626));

	logger::info("[Upscaling] Installed hooks");
}

#undef I18N_KEY_PREFIX

Upscaling::UpscaleMethod Upscaling::GetUpscaleMethod() const
{
	auto* streamline = Streamline::GetSingleton();
	auto method = static_cast<UpscaleMethod>(settings.upscaleMethod.Get());
	for (uint32_t fallback = 0; fallback < 3; ++fallback) {
		if (method == UpscaleMethod::kDLSS && !streamline->IsDLSSSupported()) {
			method = static_cast<UpscaleMethod>(settings.upscaleMethodNoDLSS.Get());
			if (method == UpscaleMethod::kDLSS)
				method = UpscaleMethod::kFSR;
			continue;
		}
		if (method == UpscaleMethod::kXeSS && !streamline->IsXeSSSupported()) {
			method = UpscaleMethod::kFSR;
			continue;
		}
		if (method == UpscaleMethod::kFSR && !streamline->IsFSRSupported())
			return UpscaleMethod::kTAA;
		return method;
	}
	return UpscaleMethod::kTAA;
}

void Upscaling::ApplyHardwareDefaults()
{
	static bool applied = false;
	if (applied)
		return;
	applied = true;

	if (settings.hardwareDefaultsApplied)
		return;
	settings.hardwareDefaultsApplied = true;

	auto* sl = Streamline::GetSingleton();

	if (settings.upscaleMethod == (uint)UpscaleMethod::kFSR) {
		if (sl->IsDLSSSupported()) {
			settings.upscaleMethod = (uint)UpscaleMethod::kDLSS;
			settings.upscaleMethodNoDLSS = (uint)UpscaleMethod::kFSR;
			logger::info("[Upscaling] Hardware default: DLSS selected (NVIDIA GPU detected)");
		} else if (sl->IsXeSSSupported()) {
			settings.upscaleMethod = (uint)UpscaleMethod::kXeSS;
			logger::info("[Upscaling] Hardware default: XeSS selected (Intel GPU detected via Streamline)");
		}
	}

	// The hardware defaults leave Reflex's low-latency mode off. Streamline::Initialize still loads the feature, for frame generation.
}

Upscaling::FrameGenMethod Upscaling::GetFrameGenMethod() const
{
	auto* sl = Streamline::GetSingleton();
	const auto selected = static_cast<FrameGenMethod>(settings.frameGenMethod.Get());
	if (selected == FrameGenMethod::kDLSSG)
		return sl->IsDLSSGSupported() ? FrameGenMethod::kDLSSG : FrameGenMethod::kFSR;
	return sl->IsFSRFGSupported() ? FrameGenMethod::kFSR :
	       sl->IsDLSSGSupported() ? FrameGenMethod::kDLSSG :
	                                FrameGenMethod::kFSR;
}

Upscaling::UpscaleMethod Upscaling::FrameUpscaleMethod() const
{
	if (!FollowsRevisions())
		return GetUpscaleMethod();
	static_assert(static_cast<std::uint32_t>(UpscaleMethod::kXeSS) == static_cast<std::uint32_t>(RenderGraph::Frame::Vars::Upscaler::XeSS));
	// None before the first adoption, so the first frames upscale nothing rather than evaluate into a texture not yet made.
	return static_cast<UpscaleMethod>(RenderGraph::Frame::Vars::Adopted(RenderGraph::Frame::Vars::UpscaleMethod()));
}

uint Upscaling::FrameQualityMode() const
{
	if (!FollowsRevisions())
		return settings.qualityMode.Get();
	return settings.qualityMode.Adopted();
}

bool Upscaling::IsFrameGenerationActive() const
{
	return IsFrameGenerationSelected() && FrameGen::ActivePresenter().Surface().ready;
}

bool Upscaling::IsFrameGenerationSelected() const
{
	if (!loaded || !settings.frameGeneration)
		return false;
	if (!FrameGen::ActivePresenter().Available())
		return false;
	auto fgMethod = GetFrameGenMethod();
	if (fgMethod == FrameGenMethod::kDLSSG)
		return Streamline::GetSingleton()->IsDLSSGSupported();
	return Streamline::GetSingleton()->IsFSRFGSupported();
}

void Upscaling::BeginRenderFrame()
{
	// A test run's upscale method (RequestUpscaleTest), as the menu's stepper sets it. The setting raises its event, and the
	// frame revision that carries it switches the method in with its textures and builds.
	if (const int request = s_upscaleTestRequest.exchange(-1, std::memory_order_relaxed); request >= 0) {
		settings.upscaleMethod = static_cast<uint>(request);
		logger::info("[UPTEST] upscale method {} (now {})", magic_enum::enum_name(static_cast<UpscaleMethod>(request)), magic_enum::enum_name(GetUpscaleMethod()));
	}
	// A test run's frame generation switch (RequestFrameGenTest), as the menu's stepper makes it, written the same way.
	if (const int request = s_frameGenTestRequest.exchange(-1, std::memory_order_relaxed); request >= 0) {
		settings.frameGeneration = request != 0;
		if (request != 0)
			settings.frameGenMethod = static_cast<uint>(request == 2 ? FrameGenMethod::kDLSSG : FrameGenMethod::kFSR);
		logger::info("[FGTEST] switch: frame generation {}", request == 0 ? "off" : request == 2 ? "DLSS-G" : "FSR-FG");
	}
	{  // The [FGTEST] line: frames rendered and FSR-FG's presents each second.
		static uint64_t secondTick = 0, lastTick = 0, lastPresented = 0;
		static uint32_t frames = 0, worst = 0, stalls = 0;
		const auto now = GetTickCount64();
		const auto& snapshot = Snapshot::Current();
		const bool paused = snapshot && snapshot->menus.paused;
		const auto describe = [&] {
			return std::format("presented x{}, setting {}x, fg {}, active {}, unusable {}, csMenu {}, paused {}, loaded {}", Streamline::GetSingleton()->GetFrameGenerationMultiplier(), settings.dlssgDynamic ? 0u : settings.frameGenMultiplier.Get(), settings.frameGeneration.Get(), IsFrameGenerationActive(), IsWindowUnusable(), globals::menu && globals::menu->ShouldSwallowInput(), paused, Streamline::GetSingleton()->IsDLSSGLoaded());
		};
		if (lastTick) {
			const auto frame = static_cast<uint32_t>(now - lastTick);
			worst = std::max(worst, frame);
			if (frame >= 150) {
				++stalls;
				logger::info("[FGTEST] stall {} ms; {}", frame, describe());
			}
		}
		lastTick = now;
		++frames;
		if (!secondTick)
			secondTick = now;
		if (now - secondTick >= 1000) {
			const uint64_t presented = Streamline::GetSingleton()->GetTotalPresentedFrames();
			logger::info("[FGTEST] {} fps, {} FSR-FG presents, worst {} ms, stalls {}; {}", frames, lastPresented ? presented - lastPresented : 0, worst, stalls, describe());
			lastPresented = presented;
			frames = worst = stalls = 0;
			secondTick = now;
		}
	}
	auto* dxvk = DXVKInterop::GetSingleton();
	// Report an unrecoverable ring once, not once per frame. This runs every frame, so a fault that
	// cannot be recovered -- a lost device above all -- used to re-enter recovery and log on every
	// single one: 81k "recovery failed" lines against 243k vkDeviceWaitIdle(-4) in one session.
	// frameGeneration is deliberately not cleared here either; it is persisted user configuration,
	// and IsFrameGenerationActive() already returns false while the ring is faulted.
	static bool ringRecoveryReported = false;
	if (dxvk->HasCommandRingFault()) {
		if (!dxvk->RecoverCommandRing()) {
			if (!std::exchange(ringRecoveryReported, true))
				logger::error("[Upscaling] Vulkan command-ring recovery failed; falling back to TAA");
		} else {
			ringRecoveryReported = false;
		}
	}

	auto& hdr = globals::features::hdrDisplay;
	if (hdr.loaded)
		hdr.BeginRenderFrame();
	else
		FrameGen::ActivePresenter().BeginFrame();

	// The presenter's committed format, for the hudless texture. Written every frame, so only a change is an event. Unknown means
	// no format is committed yet (at startup, and during a colour space change), so the last one stays.
	if (const auto surface = FrameGen::ActivePresenter().Surface(); surface.format != rhi::Format::Unknown) {
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		if (surface.format == rhi::Format::R8G8B8A8_UNorm)
			format = DXGI_FORMAT_R8G8B8A8_UNORM;
		else if (surface.format == rhi::Format::B8G8R8A8_UNorm)
			format = DXGI_FORMAT_B8G8R8A8_UNORM;
		else if (surface.format == rhi::Format::R10G10B10A2_UNorm)
			format = DXGI_FORMAT_R10G10B10A2_UNORM;
		RenderGraph::Frame::Vars::Set(RenderGraph::Frame::Vars::PresenterFormat(), { static_cast<std::uint32_t>(format), surface.hdr10 }, "Upscaling::BeginRenderFrame");
	}
}

bool Upscaling::GetEffectiveReflex() const
{
	if (IsFrameGenerationActive()) {
		switch (GetFrameGenMethod()) {
		case FrameGenMethod::kDLSSG:
			// Required: sl.dlss_g reports eFailReflexNotDetectedAtRuntime and generates nothing
			// without it, and Reflex's frame limiter works on this path (measured holding 41.25 fps
			// presented with DXVK's limiter disabled).
			return true;
		case FrameGenMethod::kFSR:
			// Off. FidelityFX's replacement swapchain owns present, and Reflex paces
			// through the present it owns, so the limit it is handed cannot be applied -- measured
			// 37 fps rendered against a 20.5 fps limit. Nor was there a latency benefit to weigh
			// against that: with Reflex off, on, and on+boost the frame rate and render-to-present
			// latency were identical (3.13 / 3.13 / 3.15 ms), because DXVK's SyncFrameLatency
			// already holds the queue at about one frame.
			//
			// The third reason that used to be here -- that leaving it off let DXVK's limiter take
			// the cap -- is gone with that limiter, so low-latency mode was re-tested against this
			// path's pacing jitter. It does not help. Three paired runs of the capped bench,
			// display-interval deviation:
			//
			//     mode off  2.65 / 2.16 / 1.82 ms   mean 2.21
			//     mode on   2.20 / 3.40 / 2.63 ms   mean 2.74
			//
			// Slightly worse, with the ranges overlapping. A single first run showed 2.20 against
			// 2.65 and looked like a 17% win; it was noise, which is worth recording because the
			// same shape of false positive has now appeared three times in this area.
			return false;
		default:
			break;
		}
	}
	return settings.reflexEnabled;
}

int Upscaling::GetMonitorRefreshRate() const
{
	// Re-query rather than trust the rate latched at swapchain creation. The display is still
	// settling then -- the mode changes under us during startup -- and latching that pinned the
	// frame cap for the whole session: the main menu ran at 10 fps because the target is
	// refresh/divisor and FSR-FG halves it again, so a stale 60 Hz became a 10 fps rendered cap
	// while the real rate was far higher. GetRenderedFrameRateLimit is called once per input
	// poll, so the result is cached for a second; QueryDisplayConfig is far too heavy per frame.
	static std::mutex s_mutex;
	static double s_cachedHz = 0.0;
	static std::chrono::steady_clock::time_point s_lastQuery{};

	const auto now = std::chrono::steady_clock::now();
	std::lock_guard lock(s_mutex);
	if (s_cachedHz < 1.0 || now - s_lastQuery >= std::chrono::seconds(1)) {
		s_lastQuery = now;
		double queried = 0.0;
		if (auto* chain = globals::d3d::swapChain) {
			DXGI_SWAP_CHAIN_DESC desc{};
			if (SUCCEEDED(chain->GetDesc(&desc)) && desc.OutputWindow)
				queried = GetRefreshRate(desc.OutputWindow);
		}
		if (queried < 1.0) {
			DEVMODEA dm{};
			dm.dmSize = sizeof(dm);
			if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) &&
				(dm.dmFields & DM_DISPLAYFREQUENCY) && dm.dmDisplayFrequency > 1)
				queried = static_cast<double>(dm.dmDisplayFrequency);
		}
		// Only fall back to the creation-time value while nothing live has answered.
		if (queried < 1.0)
			queried = refreshRate;

		if (queried >= 1.0) {
			const int before = static_cast<int>(std::lround(s_cachedHz));
			const int after = static_cast<int>(std::lround(queried));
			if (before != after)
				logger::info("[Upscaling] display refresh {} Hz (was {})", after, before);
			s_cachedHz = queried;
		}
	}

	return s_cachedHz >= 1.0 ? static_cast<int>(std::lround(s_cachedHz)) : 60;
}

int Upscaling::GetHighestRefreshRate() const
{
	DEVMODEA current{};
	current.dmSize = sizeof(current);
	if (!EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &current))
		return GetMonitorRefreshRate();
	DWORD best = current.dmDisplayFrequency;
	for (DWORD i = 0;; ++i) {
		DEVMODEA mode{};
		mode.dmSize = sizeof(mode);
		if (!EnumDisplaySettingsA(nullptr, i, &mode))
			break;
		if (mode.dmPelsWidth == current.dmPelsWidth && mode.dmPelsHeight == current.dmPelsHeight)
			best = std::max(best, mode.dmDisplayFrequency);
	}
	return static_cast<int>(best);
}

std::vector<int> Upscaling::FrameRateDivisorOptions(int a_refresh)
{
	// Offer only divisors that can still reach kMinTargetFps, then "Unlocked" as 0.
	std::vector<int> options;
	for (int d = 4; d >= 1; --d) {
		if (a_refresh / d >= kMinTargetFps)
			options.push_back(d);
	}
	if (options.empty())
		options.push_back(1);
	options.push_back(0);
	return options;
}

int Upscaling::ResolveFrameRateDivisor(int a_saved, int a_refresh)
{
	// Single source of truth for "which divisor is actually in force", shared by the settings UI
	// and GetTargetFrameRate so the number on screen and the number the limiter uses cannot
	// disagree. They did: a divisor saved against a faster display is not offered at a slower one,
	// the UI fell back to its own default while the limiter kept honouring the stale value, and the
	// menu ran at a fraction of the rate the setting claimed.
	if (a_saved <= 0)
		return 0;  // unlocked
	const std::vector<int> options = FrameRateDivisorOptions(a_refresh);
	for (int d : options) {
		if (d == a_saved)
			return a_saved;
	}
	// Not offered at this refresh rate: fall back to the fastest capped option, which is what the
	// stepper lands on and therefore what the user is shown.
	int best = 1;
	for (int d : options) {
		if (d > 0)
			best = std::min(best == 1 ? d : best, d);
	}
	return best;
}

uint32_t Upscaling::GetPresentModePreference() const
{
	// Match the Streamline sample, which is NVIDIA's reference for DLSS-G presentation on Vulkan.
	// donut's DeviceManager_VK.cpp builds its swapchain with
	//
	//     .setPresentMode(vsyncEnabled ? vk::PresentModeKHR::eFifo : vk::PresentModeKHR::eImmediate)
	//
	// and nothing in the sample varies that by frame-generation method or frame cap. MAILBOX is
	// never used. So the mode follows the sync interval and nothing else: 0 (tear-free) with vsync
	// on gives FIFO, 1 (tearing) with vsync off gives IMMEDIATE.
	//
	// This replaces a cap-dependent heuristic that picked MAILBOX whenever a frame cap was in
	// force. See tools/pacing/README.md -- PresentMon rates MAILBOX far better for DLSS-G, but it
	// cannot see through software flip metering, which submits the frame pair together and spaces
	// the flips below the level PresentMon observes.
	return settings.vsync ? 0u : 1u;
}

void Upscaling::UpdatePresentModePreference()
{
	if (!loaded || !DxvkLoader::IsLoaded())
		return;
	static uint32_t s_applied = UINT32_MAX;
	const uint32_t desired = GetPresentModePreference();
	if (s_applied == desired)
		return;
	const bool first = s_applied == UINT32_MAX;
	s_applied = desired;
	// Presenter::pickPresentMode reads the preference at swapchain creation, so an already-created
	// swapchain keeps the old mode until it is rebuilt. Skip the recreate on the first call, which
	// only records what Load() already pushed.
	if (first)
		FrameGen::DxvkPresenter::Get().PresetTearing(desired != 0);
	else {
		logger::info("[Upscaling] present mode preference -> {} (vsync {}, frame rate {})",
			desired ? "tearing" : "tear-free",
			settings.vsync ? "on" : "off",
			settings.frameRateLimitDivisor <= 0 ? "unlocked" : "capped");
		const auto method = !IsFrameGenerationActive()                      ? FrameGen::Method::kNone :
		                    GetFrameGenMethod() == FrameGenMethod::kDLSSG ? FrameGen::Method::kDLSSG :
		                                                                    FrameGen::Method::kFSR;
		FrameGen::PresentationRevision::Request(FrameGen::Controller::Presentation(method), "frame-rate setting changed present mode");
	}
}

double Upscaling::GetTargetFrameRate() const
{
	if (!loaded)
		return 0.0;
	const int divisor = settings.frameRateLimitDivisor;

	// Vertical sync is itself a cap at the refresh rate, so "unlocked" cannot mean unpaced here.
	// It matters because an external frame-generation layer owns the present: FFX's replacement
	// returns immediately instead of blocking on vblank, so nothing throttles the render thread.
	// Measured with vsync on, rate unlocked and FSR-FG active: presents held at 57.8 fps while the
	// render loop spun at 1131-1797 fps, burning the frame budget on frames that were never shown.
	// With frame generation off the same settings blocked correctly at ~60, which is why this only
	// surfaces on the FG path.
	if (divisor <= 0 && settings.vsync)
		return std::max(1.0, static_cast<double>(GetMonitorRefreshRate()));

	if (divisor <= 0) {
		// Unlocked means unlocked, for every frame-generation method. This used to return the
		// refresh rate for DLSS-G, reasoning that sl.dlss_g presents tear-free and so cannot exceed
		// the refresh rate anyway. That is true, but it does not justify silently capping a setting
		// the user set to unlimited: the cap lands on the PRESENTED cadence (see
		// GetRenderedFrameRateLimit), so 2x generation halved the RENDERED rate and with it the
		// input latency the user actually feels. FSR-FG never did this, which is how the
		// discrepancy was noticed. If guaranteed generation headroom is wanted, use the divisor.
		return 0.0;
	}
	const int refresh = GetMonitorRefreshRate();

	// Honour the same floor the settings UI applies when it builds the divisor list, so a saved
	// divisor that the current refresh rate cannot support does not quietly cap the game.
	//
	// The UI only offers divisors where refresh/d >= kMinTargetFps, and rewrites the setting to a
	// valid one the first time it is drawn. Until then the stale value was used as-is: a divisor
	// of 3 saved against a 165 Hz display became a 20 fps target on a 60 Hz one, and FSR-FG halves
	// the rendered rate again -- a 10 fps main menu that jumped to 60 the moment the menu was
	// opened and the setting was silently rewritten. Clamping here makes the two agree whether or
	// not the UI has been drawn.
	const int effective = ResolveFrameRateDivisor(divisor, refresh);
	if (effective != divisor) {
		static int s_reported = 0;
		if (std::exchange(s_reported, effective) != effective)
			logger::info("[Upscaling] frame-rate divisor {} not offered at {} Hz; using {} ({} fps)",
				divisor, refresh, effective, refresh / std::max(1, effective));
	}

	// Deliberately NOT rounded to a whole frame rate. The target is a submultiple of the display
	// refresh, and rounding it breaks that relationship: at 165 Hz a divisor of 4 becomes 41 fps
	// (24.390 ms) instead of 41.25 (24.242 ms, exactly four refresh intervals). That 0.61% error
	// drifts a full refresh interval roughly once a second, so a frame slips and the display shows
	// one long interval followed by a short one -- measured as ~1.4% of frames beyond twice the
	// median, with the exact-dividing divisors 1 and 3 pacing visibly tighter.
	return std::max(1.0, static_cast<double>(refresh) / effective);
}

uint32_t Upscaling::GetFixedDLSSGMultiplier() const
{
	uint32_t multiplier = std::clamp(settings.frameGenMultiplier.Get(), 2u, 6u);
	const uint32_t maxFrames = Streamline::GetSingleton()->GetDLSSGMaxFramesToGenerate();
	if (maxFrames > 0u)
		multiplier = std::min(multiplier, maxFrames + 1u);
	return multiplier;
}

double Upscaling::GetRenderedFrameRateLimit() const
{
	const double targetFps = GetTargetFrameRate();
	if (targetFps <= 0 || !IsFrameGenerationActive())
		return static_cast<double>(targetFps);

	switch (GetFrameGenMethod()) {
	case FrameGenMethod::kFSR: {
		// Handing FFX the output target undivided, the way the DLSS-G branch below does, was
		// measured and rejected. It does not pace better and it breaks the cap: against a 30 fps
		// target it delivered 59.8 fps, because Reflex then holds the render loop at 30 and FFX
		// still doubles it. Normalised for rate the jitter is unchanged -- 7.9% of the frame
		// interval divided against 8.4% undivided -- so this only moved the rate, not the cadence.
		//
		// Divide by what FFX is actually presenting per rendered frame, not by an assumed 2.
		// Frame generation being switched on does not mean it is generating: wherever the render
		// pass supplies no interpolation inputs -- the main menu, load screens -- FFX reports
		// numFramesActuallyPresented = 1 and passes frames through. Halving the cap there starved
		// the menu to half the target for generation that never happened: measured 15 fps rendered
		// against a 30 fps target, and 10 fps before the divisor clamp above.
		const uint32_t presented = Streamline::GetSingleton()->GetFrameGenerationMultiplier();
		return static_cast<double>(targetFps) / static_cast<double>(std::max(1u, presented));
	}
	case FrameGenMethod::kDLSSG:
		// Unlike FFX -- whose replacement swapchain owns the present loop, so the limiter only
		// ever sees rendered frames -- sl.dlss_g emits its generated frame from inside the same
		// present that Reflex's frame-limit interval (and DXVK's limiter behind Streamline)
		// throttles. The limit therefore lands on the PRESENTED cadence, and pre-dividing by the
		// multiplier here divides a second time: measured 41 rendered / 82.6 presented against a
		// 165 Hz target, i.e. target / multiplier^2. Hand the limiter the output target and let
		// DLSS-G's own pacing derive the rendered cadence from it.
		return static_cast<double>(targetFps);
	default:
		return static_cast<double>(targetFps);
	}
}

HRESULT Upscaling::PresentWithFrameGeneration(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags,
	const std::function<HRESULT(IDXGISwapChain*, UINT, UINT)>& a_present)
{
	auto* dxvk = DXVKInterop::GetSingleton();
	auto* streamline = Streamline::GetSingleton();
	auto requestFaultTeardown = [&](const char* a_reason) -> HRESULT {
		logger::error("[Upscaling] {} - disabling frame generation", a_reason);
		settings.frameGeneration = false;
		const auto display = Util::DisplaySize();
		const uint32_t displayWidth = static_cast<uint32_t>(display.x);
		const uint32_t displayHeight = static_cast<uint32_t>(display.y);
		if (streamline->IsDLSSGLoaded() &&
			!streamline->SetDLSSGMode(false, displayWidth, displayHeight)) {
			logger::error("[Upscaling] DLSS-G mode-off failed; continuing with device-idle teardown");
		}
		const bool completionProven = FrameGen::ActivePresenter().Quiesce(FrameGen::Method::kDLSSG);
		if (!completionProven) {
			// Deferring suppresses this present. That is correct for a transient fault, but only
			// while the wait can still succeed -- see the device-lost check at the top, which is
			// what stops this becoming permanent.
			static bool deferralReported = false;
			if (!std::exchange(deferralReported, true))
				logger::error("[Upscaling] frame-generation fault teardown deferred because GPU completion could not be proven");
			return DXGI_STATUS_OCCLUDED;
		}
		if (streamline->IsFSRFGLoaded() &&
			!streamline->DiscardFSRFrameGenerationPreparedFrame()) {
			logger::critical("[Upscaling] FSR prepared frame could not be discarded safely; present blocked");
			return E_FAIL;
		}
		// Ambiguous submissions keep their Vulkan views quarantined until FFX swapchain teardown.
		streamline->SetDLSSGDesiredLoaded(false);
		streamline->SetFSRFGDesiredLoaded(false);
		FrameGen::ActivePresenter().RequestConfig(FrameGen::Controller::Presentation(FrameGen::Method::kNone), a_reason);
		FrameGen::Controller::GetSingleton()->NotifyFaultTeardownRequested();
		return a_present(a_swapChain, 0, a_flags);
	};
	if (dxvk->HasCommandRingFault()) {
		settings.frameGeneration = false;
		if (streamline->IsDLSSGLoaded() || streamline->IsFSRFGLoaded())
			return requestFaultTeardown("Vulkan frame-generation dispatch fault");
		return a_present(a_swapChain, a_syncInterval, a_flags);
	}

	if (!IsFrameGenerationActive())
		return a_present(a_swapChain, a_syncInterval, a_flags);

	auto fgMethod = GetFrameGenMethod();
	FrameGen::ActivePresenter().BeforePresent(fgMethod == FrameGenMethod::kDLSSG ? FrameGen::Method::kDLSSG : FrameGen::Method::kFSR);
	if (fgMethod != FrameGenMethod::kDLSSG) {
		// FSR-FG needs the same every-present guarantee DLSS-G gets below: on frames where the
		// render pass prepared no interpolation frame -- the main menu, load screens, anywhere
		// Main_UpdateJitter does not run -- discard whatever FFX still holds so it passes the real
		// frame through instead of interpolating stale contents onto a black screen.
		(void)streamline->EnsureFSRFGPresentState();
		return a_present(a_swapChain, a_syncInterval, a_flags);
	}

	// DLSS-G requires a valid or passthrough tag for every present -- but only once it actually owns
	// the present. Between selecting it and sl.dlss_g being loaded (a swapchain recreate apart), the
	// tag path legitimately has nothing to do, and treating that as a fault tore frame generation
	// down on every enable.
	if (!streamline->IsDLSSGLoaded())
		return a_present(a_swapChain, a_syncInterval, a_flags);

	(void)streamline->EnsureDLSSGPresentTag();
	return a_present(a_swapChain, a_syncInterval, a_flags);
}

void Upscaling::CreateUpscaledTexture()
{
	// The graph declares its own, which follows the display size and the method.
	if (UpscaledResource())
		return;

	auto renderer = CS_ENGINE_READ(Renderer, Other, globals::game::renderer);
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	main.texture->GetDesc(&texDesc);

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	// Kept once made: a method change destroys nothing, so nothing in flight can still use a released texture.
	upscaledTexture = new Texture2D(texDesc);
	Util::SetResourceName(upscaledTexture->resource.get(), "Upscaling::UpscaledTexture");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	upscaledTexture->CreateSRV(srvDesc);

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = texDesc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Texture2D.MipSlice = 0;
	upscaledTexture->CreateUAV(uavDesc);

	logger::info("[Upscaling] Created upscaled texture ({}x{}, format={})",
		texDesc.Width, texDesc.Height, static_cast<int>(texDesc.Format));
}

Texture2D* Upscaling::SelectD3D11Hudless()
{
	hudlessTexture = nullptr;
	auto& hdr = globals::features::hdrDisplay;
	const bool hdrActive = hdr.loaded && hdr.IsHDREnabledForFrame();
	const auto surface = FrameGen::ActivePresenter().Surface();
	DXGI_FORMAT presenterFormat = DXGI_FORMAT_UNKNOWN;
	if (surface.format == rhi::Format::R8G8B8A8_UNorm)
		presenterFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	else if (surface.format == rhi::Format::B8G8R8A8_UNorm)
		presenterFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
	else if (surface.format == rhi::Format::R10G10B10A2_UNorm)
		presenterFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	// The presenter publishes its format asynchronously and has none until it committed one, at startup and across each colour
	// space change. That is "not yet", so the frame has no hudless colour. So is a surface still in the other encoding.
	const auto index = ORGUpscaling::HudlessFormatIndex(hdrActive, presenterFormat, surface.hdr10);
	if (!index) {
		if (surface.format != rhi::Format::Unknown && !hdrActive && !surface.hdr10)
			logger::error("[Upscaling] Unsupported HUD-less presenter format {} (HDR10 {})", static_cast<int>(surface.format), surface.hdr10);
		return nullptr;
	}
	if (auto* made = hudlessTextures[*index])
		return hudlessTexture = made;

	// The display size: the composed HDR output's, or kFRAMEBUFFER's.
	D3D11_TEXTURE2D_DESC texDesc{};
	if (hdrActive) {
		if (!hdr.outputTexture || !hdr.outputTexture->resource)
			return nullptr;
		hdr.outputTexture->resource->GetDesc(&texDesc);
	} else {
		auto& fb = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
		if (!fb.SRV)
			return nullptr;
		winrt::com_ptr<ID3D11Resource> fbResource;
		fb.SRV->GetResource(fbResource.put());
		winrt::com_ptr<ID3D11Texture2D> fbTexture;
		if (!fbResource || FAILED(fbResource->QueryInterface(IID_PPV_ARGS(fbTexture.put()))) || !fbTexture)
			return nullptr;
		fbTexture->GetDesc(&texDesc);
	}

	texDesc.Format = ORGUpscaling::kHudlessFormats[*index];
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.CPUAccessFlags = 0;
	texDesc.MiscFlags = 0;
	// Always give the hudless target a render-target view. FFX derives the UI as (backbuffer - hudless), so the two must agree
	// in format; blitting through an RTV converts, whereas CopyResource requires an exact match.
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	auto* made = new Texture2D(texDesc);
	Util::SetResourceName(made->resource.get(), "Upscaling::HudlessTexture");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	made->CreateSRV(srvDesc);

	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = texDesc.Format;
	rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	made->CreateRTV(rtvDesc);

	logger::info("[Upscaling] Created hudless texture ({}x{}, format={})", texDesc.Width, texDesc.Height, static_cast<int>(texDesc.Format));
	hudlessTextures[*index] = made;
	return hudlessTexture = made;
}

ID3D11Texture2D* Upscaling::UpscaledResource() const
{
	return ORGUpscaling::Get().Texture(ORGUpscaling::Target::Upscaled, upscaledTexture ? upscaledTexture->resource.get() : nullptr);
}

ID3D11Texture2D* Upscaling::HudlessResource() const
{
	return ORGUpscaling::Get().Texture(ORGUpscaling::Target::Hudless, hudlessTexture ? hudlessTexture->resource.get() : nullptr);
}

bool Upscaling::CopyHudlessColor(ID3D11ShaderResourceView* a_source)
{
	// Graph modes: the copy is a pass of the UI stage's FrameGeneration step.
	if (ORGUpscaling::Get().Holds(ORGUpscaling::Target::Hudless))
		return ORGUpscaling::Get().CopyHudless(a_source);
	if (!a_source || !hudlessTexture || !hudlessTexture->rtv)
		return false;

	auto* vertexShader = GetUpscaleVS();
	auto* pixelShader = GetCopyHudlessPS();
	if (!vertexShader || !pixelShader)
		return false;

	auto* context = globals::d3d::context;
	Effects11Util::D3D11ScopedPostFxBackup backup;
	backup.Save(context);

	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vertexShader, nullptr, 0);

	D3D11_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(hudlessTexture->desc.Width);
	viewport.Height = static_cast<float>(hudlessTexture->desc.Height);
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);
	context->RSSetState(nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	context->OMSetDepthStencilState(nullptr, 0);

	ID3D11ShaderResourceView* sources[] = { a_source };
	context->PSSetShaderResources(0, ARRAYSIZE(sources), sources);
	context->PSSetShader(pixelShader, nullptr, 0);
	ID3D11RenderTargetView* output = hudlessTexture->rtv.get();
	context->OMSetRenderTargets(1, &output, nullptr);
	context->Draw(3, 0);

	sources[0] = nullptr;
	context->PSSetShaderResources(0, ARRAYSIZE(sources), sources);
	backup.Restore(context);
	backup.Release();
	return true;
}

ID3D11Resource* Upscaling::CaptureHudlessColor()
{
	// Graph modes: the adopted revision's hudless texture, which the copy pass writes. Its copy also notes the source it reads.
	if (!ORGUpscaling::Get().Holds(ORGUpscaling::Target::Hudless) && !SelectD3D11Hudless())
		return nullptr;

	auto& hdr = globals::features::hdrDisplay;
	if (hdr.loaded && hdr.IsHDREnabledForFrame() && hdr.hdrTexture && hdr.hdrTexture->srv) {
		// Compose the scene with no UI, then BLIT it into the 10-bit hudless target.
		//
		// This used to CopyResource, which requires the source and destination formats to
		// match exactly and returned nullptr on any mismatch -- and nullptr means no
		// HUDLessColor reaches FFX at all, so FFX interpolates the UI along with the scene
		// and it flashes on generated frames. Blitting converts instead of refusing, so the
		// hudless buffer always lands in the same 10-bit format as the presented backbuffer
		// that FFX subtracts it from. Both frame-generation methods take this path.
		if (!hdr.ComposeCleanCapture(hdr.hdrTexture->srv.get(), false))
			return nullptr;
		if (!hdr.outputTexture || !hdr.outputTexture->srv)
			return nullptr;
		return CopyHudlessColor(hdr.outputTexture->srv.get()) ? HudlessResource() : nullptr;
	}

	auto& fb = CS_ENGINE_READ(Renderer, CapturePoint, globals::game::renderer)->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
	if (!fb.SRV)
		return nullptr;

	return CopyHudlessColor(fb.SRV) ? HudlessResource() : nullptr;
}

ID3D11PixelShader* Upscaling::GetDepthRefractionUpscalePS()
{
	if (!depthRefractionUpscalePS) {
		logger::debug("Compiling DepthRefractionUpscalePS.hlsl");
		depthRefractionUpscalePS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/Shaders/Upscaling/DepthRefractionUpscalePS.hlsl", {}, "ps_5_0"));
	}
	return depthRefractionUpscalePS.get();
}

ID3D11PixelShader* Upscaling::GetUnderwaterMaskUpscalePS()
{
	if (!underwaterMaskUpscalePS) {
		logger::debug("Compiling UnderwaterMaskUpscalePS.hlsl");
		underwaterMaskUpscalePS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/Shaders/Upscaling/UnderwaterMaskUpscalePS.hlsl", {}, "ps_5_0"));
	}
	return underwaterMaskUpscalePS.get();
}

ID3D11VertexShader* Upscaling::GetUpscaleVS()
{
	if (!upscaleVS) {
		logger::debug("Compiling UpscaleVS.hlsl");
		upscaleVS.attach((ID3D11VertexShader*)Util::CompileShader(L"Data/Shaders/Upscaling/UpscaleVS.hlsl", {}, "vs_5_0"));
	}
	return upscaleVS.get();
}

ID3D11PixelShader* Upscaling::GetCopyHudlessPS()
{
	if (!copyHudlessPS) {
		logger::debug("Compiling CopyHudlessPS.hlsl");
		copyHudlessPS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/Shaders/Upscaling/CopyHudlessPS.hlsl", {}, "ps_5_0"));
	}
	return copyHudlessPS.get();
}

int32_t GetJitterPhaseCount(int32_t renderWidth, int32_t displayWidth)
{
	const float basePhaseCount = 8.0f;
	const int32_t jitterPhaseCount = int32_t(basePhaseCount * pow((float(displayWidth) / renderWidth), 2.0f));
	return jitterPhaseCount;
}

// Calculates a Halton-sequence value for the given index and base.
static float Halton(int32_t index, int32_t base)
{
	float f = 1.0f, result = 0.0f;

	for (int32_t currentIndex = index; currentIndex > 0;) {
		f /= (float)base;
		result = result + f * (float)(currentIndex % base);
		currentIndex = (uint32_t)(floorf((float)(currentIndex) / (float)(base)));
	}

	return result;
}

void GetJitterOffset(float* outX, float* outY, int32_t index, int32_t phaseCount)
{
	const float x = Halton((index % phaseCount) + 1, 2) - 0.5f;
	const float y = Halton((index % phaseCount) + 1, 3) - 0.5f;

	*outX = x;
	*outY = y;
}

void Upscaling::ConfigureTAA()
{
	auto upscaleMethod = FrameUpscaleMethod();

	// Force temporal AA on for every method except the explicit no-AA mode.
	Util::SetTemporal(upscaleMethod != UpscaleMethod::kNONE);
}

float2 Upscaling::FrameResolutionRatio()
{
	const auto& upscaling = globals::features::upscaling;
	return upscaling.loaded ? float2{ upscaling.dynamicResolutionWidthRatio, upscaling.dynamicResolutionHeightRatio } : float2{ 1.0f, 1.0f };
}

void Upscaling::ConfigureUpscaling(RE::BSGraphics::State* a_viewport, std::array<float, 2>& a_jitter)
{
	// The frame's method and quality: the adopted revision's, so the render size, the textures and the builds switch together.
	const auto upscaleMethod = FrameUpscaleMethod();
	const uint qualityMode = FrameQualityMode();

	// Plain D3D11 makes its upscaled texture on first need. The graph's follows its declaration.
	if (upscaleMethod == UpscaleMethod::kFSR || upscaleMethod == UpscaleMethod::kDLSS || upscaleMethod == UpscaleMethod::kXeSS)
		CreateUpscaledTexture();
	FrameGen::Controller::GetSingleton()->Reconcile();

	projectionPosScaleX = a_viewport->projectionPosScaleX;
	projectionPosScaleY = a_viewport->projectionPosScaleY;

	auto state = globals::state;
	// The full display size: the adopted DisplaySize frame variable.
	const float2 screenSize = Util::DisplaySize();

	auto screenWidth = static_cast<int>(screenSize.x);
	auto screenHeight = static_cast<int>(screenSize.y);
	RenderGraph::Frame::Vars::Extent2D renderSize{ static_cast<std::uint32_t>(screenWidth), static_cast<std::uint32_t>(screenHeight) };

	if (upscaleMethod == UpscaleMethod::kFSR || upscaleMethod == UpscaleMethod::kXeSS || upscaleMethod == UpscaleMethod::kDLSS) {
		auto getUpscaleRatio = [](uint a_quality) -> float {
			switch (a_quality) {
			case 0:
				return 1.0f;  // Native (Quality)
			case 1:
				return 1.5f;  // Quality
			case 2:
				return 1.7f;  // Balanced
			case 3:
				return 2.0f;  // Performance
			case 4:
				return 3.0f;  // Ultra Performance
			default:
				return 1.5f;
			}
		};
		float resolutionScaleBase = 1.0f / getUpscaleRatio(qualityMode);

		auto renderWidth = static_cast<int>(screenWidth * resolutionScaleBase);
		auto renderHeight = static_cast<int>(screenHeight * resolutionScaleBase);

		// Report the internal resolution whenever the quality preset actually changes it, so a
		// preset that silently fails to apply is visible without guessing from frame rate.
		{
			static int s_lastW = 0, s_lastH = 0;
			static uint s_lastQuality = UINT_MAX;
			if (s_lastW != renderWidth || s_lastH != renderHeight || s_lastQuality != qualityMode) {
				s_lastW = renderWidth; s_lastH = renderHeight; s_lastQuality = qualityMode;
				logger::info("[Upscaling] internal resolution {}x{} from {}x{} (quality {}, ratio {:.2f}x, method {})",
					renderWidth, renderHeight, screenWidth, screenHeight, qualityMode,
					getUpscaleRatio(qualityMode), static_cast<uint>(upscaleMethod));
			}
		}
		resolutionScale.x = static_cast<float>(renderWidth) / static_cast<float>(screenWidth);
		resolutionScale.y = static_cast<float>(renderHeight) / static_cast<float>(screenHeight);
		renderSize = { static_cast<std::uint32_t>(renderWidth), static_cast<std::uint32_t>(renderHeight) };

		auto phaseCount = GetJitterPhaseCount(renderWidth, screenWidth);

		GetJitterOffset(&jitter.x, &jitter.y, state->frameCount, phaseCount);

		a_jitter[0] = -2.0f * jitter.x / renderWidth;
		a_jitter[1] = 2.0f * jitter.y / renderHeight;
	} else {
		resolutionScale = { 1.0f, 1.0f };

		jitter.x = -a_jitter[0] * screenWidth / 2.0f;

		jitter.y = a_jitter[1] * screenHeight / 2.0f;
	}
	// Native keeps its jitter render-side (the frame camera): game-side picking and culling see an unjittered projection.
	if (!Snapshot::FrameCamera::Owned()) {
		a_viewport->projectionPosScaleX = a_jitter[0];
		a_viewport->projectionPosScaleY = a_jitter[1];
	}

	auto& runtimeData = a_viewport->GetRuntimeData();

	runtimeData.dynamicResolutionPreviousWidthRatio = dynamicResolutionWidthRatio;
	runtimeData.dynamicResolutionPreviousHeightRatio = dynamicResolutionHeightRatio;
	runtimeData.dynamicResolutionWidthRatio = resolutionScale.x;
	runtimeData.dynamicResolutionHeightRatio = resolutionScale.y;

	dynamicResolutionWidthRatio = resolutionScale.x;
	dynamicResolutionHeightRatio = resolutionScale.y;

	runtimeData.dynamicResolutionLock = 1;

	// Written every frame. Only a changed value is an event (plan, step 5b). The render size and ratios are this frame's. The
	// method is the one the settings choose now, which the next revision carries.
	{
		namespace Vars = RenderGraph::Frame::Vars;
		constexpr std::string_view writer = "Upscaling::ConfigureUpscaling";
		const auto method = static_cast<Vars::Upscaler>(GetUpscaleMethod());
		const bool hdr = globals::features::hdrDisplay.loaded && globals::features::hdrDisplay.IsHDREnabledForFrame();
		Vars::Set(Vars::RenderSize(), renderSize, writer);
		Vars::Set(Vars::DynamicRatios(), { resolutionScale.x, resolutionScale.y }, writer);
		Vars::Set(Vars::UpscaleMethod(), method, writer);
		Vars::Set(Vars::UpscalerOptionsVar(), { method, settings.qualityMode.Get(), { static_cast<std::uint32_t>(screenWidth), static_cast<std::uint32_t>(screenHeight) }, hdr }, writer);
	}

	static float s_loggedScale = -1.0f;
	static int s_loggedMethod = -1;
	if (std::abs(resolutionScale.x - s_loggedScale) > 0.001f || (int)upscaleMethod != s_loggedMethod) {
		s_loggedScale = resolutionScale.x;
		s_loggedMethod = (int)upscaleMethod;
		logger::debug("[Upscaling] active method={} scale={:.3f} render={}x{} display={}x{}",
			(int)upscaleMethod, resolutionScale.x,
			(int)(screenSize.x * resolutionScale.x), (int)(screenSize.y * resolutionScale.y),
			(int)screenSize.x, (int)screenSize.y);
	}
}

void Upscaling::SetupResources()
{
	auto renderer = CS_ENGINE_READ(Renderer, Other, globals::game::renderer);
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
	depthStencilDesc.DepthEnable = true;
	depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthStencilDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
	depthStencilDesc.StencilEnable = false;

	DX::ThrowIfFailed(globals::d3d::device->CreateDepthStencilState(&depthStencilDesc, upscaleDepthStencilState.put()));
	Util::SetResourceName(upscaleDepthStencilState.get(), "Upscaling::DepthUpscaleDepthStencilState");

	jitterCB = new ConstantBuffer(ConstantBufferDesc<JitterCB>(), "Upscaling::JitterCB");

	D3D11_BLEND_DESC blendDesc = {};
	blendDesc.AlphaToCoverageEnable = false;
	blendDesc.IndependentBlendEnable = false;
	blendDesc.RenderTarget[0].BlendEnable = false;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&blendDesc, upscaleBlendState.put()));
	Util::SetResourceName(upscaleBlendState.get(), "Upscaling::DepthUpscaleBlendState");

	D3D11_RASTERIZER_DESC rasterizerDesc = {};
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.FrontCounterClockwise = false;
	rasterizerDesc.DepthBias = 0;
	rasterizerDesc.DepthBiasClamp = 0.0f;
	rasterizerDesc.SlopeScaledDepthBias = 0.0f;
	rasterizerDesc.DepthClipEnable = false;
	rasterizerDesc.ScissorEnable = false;
	rasterizerDesc.MultisampleEnable = false;
	rasterizerDesc.AntialiasedLineEnable = false;
	DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&rasterizerDesc, upscaleRasterizerState.put()));
	Util::SetResourceName(upscaleRasterizerState.get(), "Upscaling::DepthUpscaleRasterizerState");

	// Graph modes declare their textures and clients here. Plain D3D11 makes its textures on first need.
	ORGUpscaling::Get().Setup();

	auto* dxvk = DXVKInterop::GetSingleton();
	if (dxvk->Initialize()) {
		VkImage probeImage = VK_NULL_HANDLE;
		VkImageCreateInfo probeInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		if (dxvk->GetVkImage(main.texture, &probeImage, nullptr, &probeInfo) && probeImage != VK_NULL_HANDLE) {
			logger::info("[Upscaling] DXVK texture->VkImage probe OK: main RT VkImage={:#x} ({}x{}, vkFormat={})",
				reinterpret_cast<uintptr_t>(probeImage), probeInfo.extent.width, probeInfo.extent.height, static_cast<int>(probeInfo.format));
		} else {
			logger::warn("[Upscaling] DXVK texture->VkImage probe FAILED");
		}

		dxvk->CreateCommandResources(3);

		auto* streamline = Streamline::GetSingleton();
		if (streamline->Initialize()) {
			streamline->SetVulkanDevice();
			FrameGen::ActivePresenter().SetTornDown(&Streamline::CompleteSwapchainTeardown);
		}

		ApplyHardwareDefaults();
	} else if (RenderGraph::Engine::Native::Active()) {
		// Native mode: Streamline came up with the native device (NativeDevice's NativeStreamline); its support is the device's adapter's.
		DxvkOrgInteropDeviceInfo vulkan{};
		const bool onVulkan = RenderGraph::Engine::Native::DescribeVulkanDevice(vulkan);
		Streamline::GetSingleton()->SetNativeDevice(onVulkan ? static_cast<void*>(vulkan.instance) : nullptr,
			onVulkan ? reinterpret_cast<void*>(vulkan.getInstanceProcAddr) : nullptr, onVulkan ? static_cast<void*>(vulkan.physicalDevice) : nullptr,
			RenderGraph::Engine::Native::AdapterLuid());
		// Frame generation's plugins load and unload between a replacement's destroy and create.
		FrameGen::ActivePresenter().SetTornDown(&Streamline::CompleteSwapchainTeardown);
		ApplyHardwareDefaults();
	}
}

void Upscaling::ClearShaderCache()
{
	depthRefractionUpscalePS = nullptr;
	underwaterMaskUpscalePS = nullptr;
	upscaleVS = nullptr;
	copyHudlessPS = nullptr;
}

void UpdateCameraData()
{
	using func_t = decltype(&UpdateCameraData);
	static REL::Relocation<func_t> func{ RELOCATION_ID(75472, 77258) };
	func();
}

void Upscaling::PostDisplay()
{
	// Writes engine state: the undo of the jitter and dynamic resolution ConfigureUpscaling wrote (render-owned jitter, phase 2).
	auto viewport = CS_ENGINE_READ(GraphicsState, Other, globals::game::graphicsState);

	viewport->projectionPosScaleX = projectionPosScaleX;
	viewport->projectionPosScaleY = projectionPosScaleY;

	auto& runtimeData = viewport->GetRuntimeData();

	runtimeData.dynamicResolutionPreviousWidthRatio = 1;
	runtimeData.dynamicResolutionPreviousHeightRatio = 1;
	runtimeData.dynamicResolutionWidthRatio = 1;
	runtimeData.dynamicResolutionHeightRatio = 1;
	runtimeData.dynamicResolutionLock = 1;

	CS_ENGINE_READ(Renderer, Other, globals::game::renderer)->UpdateViewPort(0, 0, 1);
	UpdateCameraData();
	Snapshot::FrameCamera::OnFullResolution();

	globals::state->UpdateSharedData(false);
}

/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

double Upscaling::GetRefreshRate(HWND a_window)
{
	HMONITOR monitor = MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW info;
	info.cbSize = sizeof(info);
	if (GetMonitorInfoW(monitor, &info) != 0) {
		UINT32 requiredPaths, requiredModes;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, &requiredModes) == ERROR_SUCCESS) {
			std::vector<DISPLAYCONFIG_PATH_INFO> paths(requiredPaths);
			std::vector<DISPLAYCONFIG_MODE_INFO> modes2(requiredModes);
			if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, paths.data(), &requiredModes, modes2.data(), nullptr) == ERROR_SUCCESS) {
				for (auto& p : paths) {
					DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName;
					sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
					sourceName.header.size = sizeof(sourceName);
					sourceName.header.adapterId = p.sourceInfo.adapterId;
					sourceName.header.id = p.sourceInfo.id;
					if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS && wcscmp(info.szDevice, sourceName.viewGdiDeviceName) == 0) {
						UINT numerator = p.targetInfo.refreshRate.Numerator;
						UINT denominator = p.targetInfo.refreshRate.Denominator;
						return (double)numerator / (double)denominator;
					}
				}
			}
		}
	}
	logger::error("Failed to retrieve refresh rate from swap chain");
	return 60;
}

bool Upscaling::IsUpscalingActive() const
{
	auto method = FrameUpscaleMethod();

	if (method != UpscaleMethod::kFSR && method != UpscaleMethod::kXeSS && method != UpscaleMethod::kDLSS) {
		return false;
	}

	return resolutionScale.x < .99f;
}

bool Upscaling::IsWindowMinimized()
{
	// Suspend all Streamline work while the swapchain cannot present.
	static HWND s_window = nullptr;
	if (!s_window && globals::d3d::swapChain) {
		DXGI_SWAP_CHAIN_DESC desc{};
		if (SUCCEEDED(globals::d3d::swapChain->GetDesc(&desc)))
			s_window = desc.OutputWindow;
	}
	return s_window && IsIconic(s_window);
}

void Upscaling::RequestUpscaleTest(int a_method)
{
	s_upscaleTestRequest.store(a_method, std::memory_order_relaxed);
}

void Upscaling::RequestFrameGenTest(int a_method)
{
	s_frameGenTestRequest.store(a_method, std::memory_order_relaxed);
}

void Upscaling::NotifyWindowFocus(bool a_focused)
{
	s_windowUnfocused.store(!a_focused, std::memory_order_relaxed);
}

void Upscaling::NotifyWindowModifying(bool a_modifying)
{
	s_windowModifying.store(a_modifying, std::memory_order_relaxed);
}

void Upscaling::NotifyDisplayState(bool a_on)
{
	// Windows reports the current state on registering, which is not a change.
	if (s_displayOff.exchange(!a_on, std::memory_order_relaxed) == !a_on)
		return;
	logger::info("[Upscaling] the display turned {}", a_on ? "on" : "off");
}

namespace
{
	/** @brief The window state now: minimized, unfocused (unless CS_UPSCALE_UNFOCUSED=1), or being sized or moved. */
	bool ReadWindowUnusable()
	{
		if (const auto foregroundWindow = GetForegroundWindow()) {
			DWORD foregroundProcess = 0;
			GetWindowThreadProcessId(foregroundWindow, &foregroundProcess);
			if (foregroundProcess != 0)
				Upscaling::s_windowUnfocused.store(foregroundProcess != GetCurrentProcessId(), std::memory_order_relaxed);
		}

		// CS_UPSCALE_UNFOCUSED=1: unattended test runs on another monitor keep upscaling (and HDR, frame generation) while another
		// process has the foreground; minimized and size/move still suspend.
		static const bool s_ignoreFocus = Switches::On("CS_UPSCALE_UNFOCUSED");

		return Upscaling::IsWindowMinimized() ||
		       (!s_ignoreFocus && Upscaling::s_windowUnfocused.load(std::memory_order_relaxed)) ||
		       Upscaling::s_windowModifying.load(std::memory_order_relaxed);
	}
}

void Upscaling::LatchWindowState()
{
	s_windowUnusableLatched.store(ReadWindowUnusable(), std::memory_order_relaxed);
	s_windowLatched.store(true, std::memory_order_release);
}

bool Upscaling::IsWindowUnusable()
{
	// Once frames begin, a frame reads one answer from its Begin to the next. Before the first Begin, the window is read now.
	if (s_windowLatched.load(std::memory_order_acquire))
		return s_windowUnusableLatched.load(std::memory_order_relaxed);
	return ReadWindowUnusable();
}

void Upscaling::SeparateFromGraphEpochs()
{
	// The upscalers run through Streamline on the descriptor heap path (a Streamline plugin asks for VK_EXT_descriptor_heap, which the
	// render graph has enabled on DXVK's device). Their command buffer must not run directly after a render-graph epoch's command buffer,
	// which binds the graph's descriptor heaps: on NVIDIA's driver the evaluate then computes from the wrong descriptors whenever the two
	// run back to back on the GPU, and writes black or garbage output (docs/development/render-graph.md, "Hazard: external Vulkan work
	// directly after a graph epoch"; found at the Sleeping Giant Inn, where Drawcall Limit Fix's first-person colour epoch is the last epoch
	// before the upscaler). Frame generation's evaluates do not call this yet. DXVK work that binds image descriptors in between avoids it: a UAV
	// clear of a small typed texture, which DXVK runs as a compute shader. A plain flush, an empty submission or a buffer fill does not.
	// Native mode has no DXVK command stream to separate: its evaluates are graph work in the epoch's own list.
	if (!RenderGraphRuntime::Get().IsActive() || RenderGraph::Engine::Native::Active())
		return;
	if (!epochSeparator) {
		D3D11_TEXTURE2D_DESC desc{ 64, 64, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, texture.put())) ||
			FAILED(globals::d3d::device->CreateUnorderedAccessView(texture.get(), nullptr, epochSeparator.put()))) {
			logger::error("[Upscaling] The graph epoch separator could not be created; the upscaler may follow an epoch directly");
			return;
		}
		Util::SetResourceName(texture.get(), "Upscaling::EpochSeparator");
	}
	const UINT values[4] = { 1, 2, 3, 4 };
	globals::d3d::context->ClearUnorderedAccessViewUint(epochSeparator.get(), values);
}

void Upscaling::Upscale()
{
	ZoneScoped;

	if (IsWindowMinimized())
		return;

	auto state = globals::state;
	auto context = globals::d3d::context;
	auto renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
	const auto method = FrameUpscaleMethod();
	// The SDK options are this frame's: the adopted quality, the display size and the pre-exposure (Streamline sets them when
	// the evaluate records).
	const uint qualityMode = FrameQualityMode();

	context->OMSetRenderTargets(0, nullptr, nullptr);

	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

	if (auto* dxvk = DXVKInterop::GetSingleton(); dxvk && dxvk->IsAvailable()) {
		// This is the thread issuing D3D11's commands: its ring submissions go through DXVK's command stream.
		dxvk->BindStreamThread();
		dxvk->PublishCommandTimings();
	}

	{
		// GPU time comes from the interop buffer's own timestamps (PublishCommandTimings above); a D3D11 timer
		// around the evaluation would straddle that buffer's submission and count the queue's idle time.
		state->BeginPerfEvent("Upscaling");
		CS_GPU_ZONE(globals::state->tracyCtx, "Upscaling Dispatch");

		// Engine target fields can be tripwire handles: Streamline and DXVK take the wrapped objects.
		using RenderGraph::Engine::Tripwire::Unwrap;
		auto* mainTexture = Unwrap(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture);
		auto* depthTexture = Unwrap(renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture);
		auto* motionTexture = Unwrap(motionVector.texture);
		const auto displaySize = Util::DisplaySize();
		const auto renderSize = Util::ConvertToDynamic(displaySize);
		auto result = Streamline::EvaluationResult::kFailed;

		auto* upscaled = UpscaledResource();
		// The evaluate as the post stage's pass records inside the stage's block, after its earlier passes: there is nothing to separate,
		// and a D3D11 clear here would sit inside the stage's window.
		if (!ORGUpscaling::Get().EvaluatesInStage())
			SeparateFromGraphEpochs();
		if (upscaled) {
			switch (method) {
			case UpscaleMethod::kFSR:
				result = Streamline::GetSingleton()->EvaluateFSR(
					mainTexture, upscaled, depthTexture, motionTexture,
					(uint32_t)renderSize.x, (uint32_t)renderSize.y,
					(uint32_t)displaySize.x, (uint32_t)displaySize.y,
					qualityMode, settings.sharpnessFSR, jitter.x, jitter.y);
				break;
			case UpscaleMethod::kDLSS:
				result = Streamline::GetSingleton()->EvaluateDLSS(
					mainTexture, upscaled, depthTexture, motionTexture,
					(uint32_t)renderSize.x, (uint32_t)renderSize.y,
					(uint32_t)displaySize.x, (uint32_t)displaySize.y,
					qualityMode, jitter.x, jitter.y);
				break;
			case UpscaleMethod::kXeSS:
				result = Streamline::GetSingleton()->EvaluateXeSS(
					mainTexture, upscaled, depthTexture, motionTexture,
					(uint32_t)renderSize.x, (uint32_t)renderSize.y,
					(uint32_t)displaySize.x, (uint32_t)displaySize.y,
					qualityMode, settings.sharpnessFSR, jitter.x, jitter.y);
				break;
			default:
				result = Streamline::EvaluationResult::kSkipped;
				break;
			}
		}

		// Not timed: this is the first use of either image in DXVK's next command list, so DXVK moves the copy
		// into that list's init buffer, ahead of any timestamp a D3D11 timer here would write.
		// CS_ORG_UPSCALING: the copy is a pass at the Upscale anchor, which runs after this point (PerformUpscaling).
		if (result == Streamline::EvaluationResult::kReady && !ORGUpscaling::Get().QueueCopyToMain())
			context->CopyResource(mainTexture, upscaled);

		state->EndPerfEvent();
	}
}

void Upscaling::PerformUpscaling()
{
	ZoneScoped;
	CS_GPU_ZONE(globals::state->tracyCtx, "Upscaling");
	Upscale();
	UpscaleDepth();
	// CS_ORG_UPSCALING: the copy into kMAIN and the depth upscale queued above.
	ORGUpscaling::Get().RunUpscale();

	// Writes engine state (phase 2's render-owned jitter removes it).
	auto& runtimeData = CS_ENGINE_READ(GraphicsState, Other, globals::game::graphicsState)->GetRuntimeData();

	runtimeData.dynamicResolutionLock = 1;

	UpdateCameraData();
	Snapshot::FrameCamera::OnFullResolution();
}

Upscaling::JitterCB Upscaling::DepthUpscaleConstants()
{
	JitterCB jitterData;
	jitterData.jitter = jitter;
	constexpr float kEnterWideKernelRatio = 1.55f;
	constexpr float kExitWideKernelRatio = 1.45f;
	const float minScale = std::max(std::min(resolutionScale.x, resolutionScale.y), FLT_EPSILON);
	const float upscaleRatio = 1.0f / minScale;

	if (depthUpscaleUseWideKernel) {
		if (upscaleRatio < kExitWideKernelRatio) {
			depthUpscaleUseWideKernel = false;
		}
	} else {
		if (upscaleRatio > kEnterWideKernelRatio) {
			depthUpscaleUseWideKernel = true;
		}
	}

	jitterData.useWideKernel = depthUpscaleUseWideKernel ? 1.0f : 0.0f;
	jitterData.pad0 = 0.0f;
	return jitterData;
}

void Upscaling::UpscaleDepth()
{
	ZoneScoped;
	CS_GPU_ZONE(globals::state->tracyCtx, "Upscaling - Depth");

	if (!IsUpscalingActive()) {
		return;
	}

	auto state = globals::state;
	auto renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	if (!state || !renderer || !context || !deferred || !deferred->linearSampler || !jitterCB || !upscaleRasterizerState || !upscaleBlendState || !upscaleDepthStencilState) {
		return;
	}

	const float2 screenSize = Util::DisplaySize();
	if (screenSize.x <= 0.0f || screenSize.y <= 0.0f) {
		return;
	}

	const JitterCB jitterData = DepthUpscaleConstants();
	// CS_ORG_UPSCALING: the copies and both draws are passes at the Upscale anchor, which PerformUpscaling runs next.
	if (ORGUpscaling::Get().QueueDepthUpscale(jitterData.jitter, jitterData.useWideKernel))
		return;

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
	auto& refractionNormals = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kREFRACTION_NORMALS];
	auto& saoCameraZ = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSAO_CAMERAZ];
	auto& underwaterMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kUNDERWATER_MASK];

	if (!depth.texture || !depth.views[0] || !depthCopy.texture || !depthCopy.depthSRV ||
		!refractionNormals.texture || !refractionNormals.textureCopy || !refractionNormals.SRVCopy || !refractionNormals.RTV || !saoCameraZ.RTV ||
		!underwaterMask.texture || !underwaterMask.textureCopy || !underwaterMask.SRVCopy || !underwaterMask.RTV) {
		return;
	}
	auto* fullscreenVS = GetUpscaleVS();
	auto* depthUpscalePS = GetDepthRefractionUpscalePS();
	auto* underwaterMaskPS = GetUnderwaterMaskUpscalePS();
	if (!fullscreenVS || !depthUpscalePS || !underwaterMaskPS) {
		return;
	}

	state->BeginPerfEvent("Render Target Upscaling");

	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	context->VSSetShader(fullscreenVS, nullptr, 0);

	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = screenSize.x;
	viewport.Height = screenSize.y;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	context->RSSetState(upscaleRasterizerState.get());
	context->OMSetBlendState(upscaleBlendState.get(), nullptr, 0xffffffff);

	ID3D11SamplerState* samplers[] = { deferred->linearSampler };
	context->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	jitterCB->Update(jitterData);
	auto bufferArray = jitterCB->CB();
	context->PSSetConstantBuffers(0, 1, &bufferArray);

	// Engine target fields can be tripwire handles: DXVK's context takes the wrapped objects.
	using RenderGraph::Engine::Tripwire::Unwrap;
	const auto copyIfNonAliased = [&](ID3D11Resource* dst, ID3D11Resource* src) {
		dst = Unwrap(dst);
		src = Unwrap(src);
		if (dst && src && dst != src) {
			context->CopyResource(dst, src);
		}
	};

	{
		CS_GPU_ZONE(globals::state->tracyCtx, "Upscaling - Depth Upscale");

		copyIfNonAliased(depthCopy.texture, depth.texture);

		context->OMSetDepthStencilState(upscaleDepthStencilState.get(), 0x00);

		copyIfNonAliased(refractionNormals.textureCopy, refractionNormals.texture);

		ID3D11ShaderResourceView* srvs[] = { Unwrap(refractionNormals.SRVCopy), Unwrap(depthCopy.depthSRV), Unwrap(depthCopy.stencilSRV) };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11RenderTargetView* rtvs[] = { Unwrap(refractionNormals.RTV), Unwrap(saoCameraZ.RTV) };
		context->OMSetRenderTargets(2, rtvs, Unwrap(depth.views[0]));

		context->PSSetShader(depthUpscalePS, nullptr, 0);
		globals::profiler->BeginPass("Upscaling::DepthUpscale");
		context->Draw(3, 0);
		globals::profiler->EndPass();
	}

	{
		CS_GPU_ZONE(globals::state->tracyCtx, "Upscaling - Underwater Mask");

		viewport.Width = screenSize.x * 0.5f;
		viewport.Height = screenSize.y * 0.5f;
		context->RSSetViewports(1, &viewport);

		copyIfNonAliased(underwaterMask.textureCopy, underwaterMask.texture);

		context->OMSetDepthStencilState(nullptr, 0x00);

		ID3D11ShaderResourceView* srvs[] = { Unwrap(underwaterMask.SRVCopy), Unwrap(depthCopy.depthSRV) };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11RenderTargetView* rtvs[] = { Unwrap(underwaterMask.RTV) };
		context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);

		context->PSSetShader(underwaterMaskPS, nullptr, 0);
		globals::profiler->BeginPass("Upscaling::UnderwaterMaskUpscale");
		context->Draw(3, 0);
		globals::profiler->EndPass();
	}

	ID3D11ShaderResourceView* nullPSResources[3] = { nullptr, nullptr, nullptr };
	context->PSSetShaderResources(0, ARRAYSIZE(nullPSResources), nullPSResources);

	state->EndPerfEvent();
}

void Upscaling::PrepareFrameGeneration(ID3D11Resource* a_hudlessColor)
{
	const auto& snapshot = Snapshot::Current();
	// A snapshot before the first game update (id 0) is the startup's: no gameplay yet.
	const bool gameplay = snapshot && snapshot->id != 0 && !snapshot->menus.paused && !snapshot->menus.main && !snapshot->menus.loading;
	const auto fgMethod = GetFrameGenMethod();
	if (!FrameGen::ActivePresenter().Surface().ready ||
		(fgMethod == FrameGenMethod::kFSR &&
		 !FrameGen::Controller::GetSingleton()->IsFSRPresenterReady()))
		return;

	auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
	// Engine target fields can be tripwire handles: Streamline takes the wrapped objects.
	using RenderGraph::Engine::Tripwire::Unwrap;
	auto* motionTexture = Unwrap(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].texture);

	if (fgMethod == FrameGenMethod::kDLSSG) {
		FrameGen::Controller::GetSingleton()->EngageDLSSG();

		if (gameplay) {
			const auto displaySize = Util::DisplaySize();
			const auto renderSize = Util::ConvertToDynamic(displaySize, true);
			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			auto& depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
			const bool depthIsCopy = IsUpscalingActive() && depthCopy.texture;
			ID3D11Resource* fgDepth = Unwrap(depthIsCopy ? depthCopy.texture : depth.texture);

			// Guest gives DLSS-G an evaluate a frame without an upscaler. Native sets the frame's constants in the inputs pass instead.
			if (!IsUpscalingActive() && !RenderGraph::Engine::Native::Active()) {
				CreateUpscaledTexture();
				auto& mainColor = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
				// The graph's upscaled texture is one texel until the revision that turns frame generation on is adopted.
				const auto covers = [&](ID3D11Texture2D* a_texture) {
					D3D11_TEXTURE2D_DESC desc{};
					a_texture->GetDesc(&desc);
					return desc.Width >= static_cast<UINT>(displaySize.x) && desc.Height >= static_cast<UINT>(displaySize.y);
				};
				if (auto* upscaled = UpscaledResource(); upscaled && mainColor.texture && covers(upscaled))
					(void)Streamline::GetSingleton()->EvaluateFSR(
						Unwrap(mainColor.texture), upscaled, fgDepth, motionTexture,
						(uint32_t)displaySize.x, (uint32_t)displaySize.y, (uint32_t)displaySize.x, (uint32_t)displaySize.y,
						0, 0.0f, jitter.x, jitter.y);
			}

			static uint32_t s_lastTagFrame = UINT32_MAX;
			const uint32_t tagFrame = globals::state->frameCount;
			if (s_lastTagFrame != tagFrame) {
				s_lastTagFrame = tagFrame;
				Streamline::GetSingleton()->TagDLSSGResources(
					fgDepth, motionTexture, a_hudlessColor,
					(uint32_t)renderSize.x, (uint32_t)renderSize.y,
					(uint32_t)displaySize.x, (uint32_t)displaySize.y, jitter.x, jitter.y, depthIsCopy);
			}
		}
	} else if (fgMethod == FrameGenMethod::kFSR && gameplay) {
		const auto displaySize = Util::DisplaySize();
		const auto renderSize = Util::ConvertToDynamic(displaySize, true);
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
		const bool depthIsCopy = IsUpscalingActive() && depthCopy.texture;
		ID3D11Resource* fgDepth = Unwrap(depthIsCopy ? depthCopy.texture : depth.texture);
		(void)Streamline::GetSingleton()->EvaluateFSRFrameGen(
			fgDepth, motionTexture, a_hudlessColor,
			(uint32_t)renderSize.x, (uint32_t)renderSize.y,
			(uint32_t)displaySize.x, (uint32_t)displaySize.y,
			jitter.x, jitter.y, depthIsCopy);
		Streamline::GetSingleton()->CaptureFSRFrameGenState();
	}
}

void Upscaling::Main_UpdateJitter::thunk(RE::BSGraphics::State* a_state)
{
	auto& upscaling = globals::features::upscaling;
	upscaling.BeginRenderFrame();
	Streamline::GetSingleton()->BeginRenderFrame();
	upscaling.ConfigureTAA();
	std::array<float, 2> jitter{};
	if (RenderGraph::Frame::PostChain::OwnsJitter())
		jitter = RenderGraph::Frame::PostChain::UpdateJitter(a_state);
	else {
		func(a_state);
		jitter = { a_state->projectionPosScaleX, a_state->projectionPosScaleY };
	}
	// ConfigureUpscaling moves this frame's ratios into the previous ones.
	const float previousWidth = upscaling.dynamicResolutionWidthRatio, previousHeight = upscaling.dynamicResolutionHeightRatio;
	upscaling.ConfigureUpscaling(a_state, jitter);
	RenderGraph::Frame::PostChain::NoteJitter(jitter[0], jitter[1]);
	Snapshot::FrameCamera::BeginRender(jitter, { upscaling.dynamicResolutionWidthRatio, upscaling.dynamicResolutionHeightRatio, previousWidth, previousHeight });
}

void Upscaling::MenuManagerDrawInterfaceStartHook::thunk(int64_t a1)
{
	auto& upscaling = globals::features::upscaling;

	upscaling.PostDisplay();
	globals::features::screenshotFeature.SnapshotCleanScene();

	auto& hdr = globals::features::hdrDisplay;
	if (hdr.loaded)
		hdr.SetUIBuffer();

	func(a1);
}

void Upscaling::Main_PostProcessing::thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5)
{
	FrameAnchors::Run(FrameAnchors::Anchor::PostProcessing);

	auto& postProcessing = globals::features::postProcessing;
	if (postProcessing.loaded) {
		postProcessing.DrawBeforeUpscaling();
	}

	auto& upscaling = globals::features::upscaling;
	auto upscaleMethod = upscaling.FrameUpscaleMethod();

	// Keep markers, evaluation, tagging, and present on the same usable-window frame.
	const bool windowUsable = !Upscaling::IsWindowUnusable();

	auto* streamline = Streamline::GetSingleton();
	// SimulationEnd once per snapshot (a repeated render has no new simulation), RenderSubmitStart once per render frame.
	if (windowUsable && upscaling.GetEffectiveReflex()) {
		static std::uint64_t s_lastSimEndSnapshot = 0;
		static std::uint64_t s_lastSubmitFrame = UINT64_MAX;
		const auto& snapshot = Snapshot::Current();
		const std::uint64_t renderFrame = Snapshot::Render().frame;
		if (s_lastSubmitFrame != renderFrame) {
			s_lastSubmitFrame = renderFrame;
			if (snapshot && s_lastSimEndSnapshot != snapshot->id) {
				s_lastSimEndSnapshot = snapshot->id;
				streamline->SetPCLMarker(Streamline::PclMarker::SimulationEnd);
			}
			streamline->SetPCLMarker(Streamline::PclMarker::RenderSubmitStart);
		}
	}

	if (windowUsable && (upscaleMethod == UpscaleMethod::kFSR || upscaleMethod == UpscaleMethod::kXeSS || upscaleMethod == UpscaleMethod::kDLSS))
		upscaling.PerformUpscaling();

	Util::SetTemporal(upscaleMethod == UpscaleMethod::kTAA);

	bool hdrLoaded = globals::features::hdrDisplay.loaded;
	if (hdrLoaded)
		globals::features::hdrDisplay.RedirectFramebuffer();

	func(a_this, a3, a_target, a_4, a_5);

	if (hdrLoaded)
		globals::features::hdrDisplay.RestoreFramebuffer();

	// kFRAMEBUFFER holds the finished scene, before the UI: the render graph's screenshot copy takes it here.
	globals::features::screenshotFeature.SnapshotCleanSceneOnGraph();

	if (windowUsable && upscaling.IsFrameGenerationActive()) {
		const auto fgMethod = upscaling.GetFrameGenMethod();
		const bool presenterReady = FrameGen::ActivePresenter().Surface().ready;
		const bool fsrReady = fgMethod != FrameGenMethod::kFSR ||
		                      FrameGen::Controller::GetSingleton()->IsFSRPresenterReady();
		if (presenterReady && fsrReady)
			upscaling.PrepareFrameGeneration(upscaling.CaptureHudlessColor());
	}

	Util::SetTemporal(false);
}

void Upscaling::Main_RenderPrecipitation::thunk()
{
	// Native's occlusion maps make no engine render call (Snapshot::PrecipitationOcclusion), so they need no lock. The unlock is still
	// needed: ConfigureUpscaling locks dynamic resolution at the frame's start, and the world drawn after this point is scaled.
	// Native readers of the lock (ImageSpace, Util::ConvertToDynamic) would take the frame as full resolution otherwise.
	if (RenderGraph::Engine::Native::Active()) {
		func();
		CS_ENGINE_READ(GraphicsState, Other, globals::game::graphicsState)->GetRuntimeData().dynamicResolutionLock = 0;
		return;
	}
	// Writes engine state around an engine render function (goes with phase 3's port of it).
	auto& runtimeData = CS_ENGINE_READ(GraphicsState, Other, globals::game::graphicsState)->GetRuntimeData();
	runtimeData.dynamicResolutionLock = 1;
	func();
	runtimeData.dynamicResolutionLock = 0;
}

void Upscaling::BSFaceGenManager_UpdatePendingCustomizationTextures::thunk()
{
	// Writes engine state around an engine render function (goes with phase 3's port of it).
	auto& runtimeData = CS_ENGINE_READ(GraphicsState, Other, globals::game::graphicsState)->GetRuntimeData();
	runtimeData.dynamicResolutionLock = 1;
	func();
	runtimeData.dynamicResolutionLock = 0;
}

#include "ViewAtmosphere.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/IBL.h"
#include "Features/LightLimitFix.h"
#include "Features/PhysicalSky.h"
#include "Features/Skylighting.h"
#include "Features/TerrainShadows.h"
#include "Features/ViewAtmosphere/ORGViewAtmosphere.h"
#include "Features/ViewAtmosphere/ORGViewAtmosphereReflection.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"
#include "Utils/DebugSection.h"
#include "Utils/GpuBlock.h"

#define I18N_KEY_PREFIX "feature.view_atmosphere."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ViewAtmosphere::Settings,
	enabled,
	nearPixelSize,
	nearSlices,
	middlePixelSize,
	middleSlices,
	distantPixelSize,
	distantSlices,
	historyMissSamples,
	nearRange,
	localRange,
	historyWeight,
	sampleJitter,
	shadowBias,
	depthDistribution,
	lookupJitter)

ViewAtmosphere::ViewAtmosphere()
{
	settings.enabled.Bind(CS::SettingRegistry::Declare("ViewAtmosphere.enabled"));
	settings.nearPixelSize.Bind(CS::SettingRegistry::Declare("ViewAtmosphere.nearPixelSize"));
	settings.nearSlices.Bind(CS::SettingRegistry::Declare("ViewAtmosphere.nearSlices"));
	settings.middlePixelSize.Bind(CS::SettingRegistry::Declare("ViewAtmosphere.middlePixelSize"));
	settings.middleSlices.Bind(CS::SettingRegistry::Declare("ViewAtmosphere.middleSlices"));
	settings.distantPixelSize.Bind(CS::SettingRegistry::Declare("ViewAtmosphere.distantPixelSize"));
	settings.distantSlices.Bind(CS::SettingRegistry::Declare("ViewAtmosphere.distantSlices"));
}

namespace
{
	using AtmosphereCpu = ViewAtmosphere::Settings;
	using AtmosphereGpu = ViewAtmosphere::GpuSettings;

	// Every GPU field comes from the setting of the same name.
	using AtmosphereGpuBlock = CS::GpuBlock<AtmosphereGpu,
		CS::GpuField<&AtmosphereCpu::enabled, &AtmosphereGpu::enabled>,
		CS::GpuField<&AtmosphereCpu::nearPixelSize, &AtmosphereGpu::nearPixelSize>,
		CS::GpuField<&AtmosphereCpu::nearSlices, &AtmosphereGpu::nearSlices>,
		CS::GpuField<&AtmosphereCpu::middlePixelSize, &AtmosphereGpu::middlePixelSize>,
		CS::GpuField<&AtmosphereCpu::middleSlices, &AtmosphereGpu::middleSlices>,
		CS::GpuField<&AtmosphereCpu::distantPixelSize, &AtmosphereGpu::distantPixelSize>,
		CS::GpuField<&AtmosphereCpu::distantSlices, &AtmosphereGpu::distantSlices>,
		CS::GpuField<&AtmosphereCpu::historyMissSamples, &AtmosphereGpu::historyMissSamples>,
		CS::GpuField<&AtmosphereCpu::nearRange, &AtmosphereGpu::nearRange>,
		CS::GpuField<&AtmosphereCpu::localRange, &AtmosphereGpu::localRange>,
		CS::GpuField<&AtmosphereCpu::historyWeight, &AtmosphereGpu::historyWeight>,
		CS::GpuField<&AtmosphereCpu::sampleJitter, &AtmosphereGpu::sampleJitter>,
		CS::GpuField<&AtmosphereCpu::shadowBias, &AtmosphereGpu::shadowBias>,
		CS::GpuField<&AtmosphereCpu::depthDistribution, &AtmosphereGpu::depthDistribution>,
		CS::GpuField<&AtmosphereCpu::lookupJitter, &AtmosphereGpu::lookupJitter>>;
}

ViewAtmosphere::GpuSettings ViewAtmosphere::GetCommonBufferData() const
{
	return AtmosphereGpuBlock::Fill(settings);
}

void ViewAtmosphere::LoadSettings(json& value)
{
	settings = value;
	const Settings defaults;
	const auto bounded = [](float input, float fallback, float minimum, float maximum) {
		return std::clamp(std::isfinite(input) ? input : fallback, minimum, maximum);
	};
	settings.enabled = settings.enabled.Get() != 0 ? 1u : 0u;
	settings.nearPixelSize = std::clamp(settings.nearPixelSize.Get(), 4u, 64u);
	settings.middlePixelSize = std::clamp(settings.middlePixelSize.Get(), settings.nearPixelSize.Get(), 64u);
	settings.distantPixelSize = std::clamp(settings.distantPixelSize.Get(), settings.nearPixelSize.Get(), 64u);
	settings.nearSlices = std::clamp(settings.nearSlices.Get(), 16u, 160u);
	settings.middleSlices = std::clamp(settings.middleSlices.Get(), 16u, 160u);
	settings.distantSlices = std::clamp(settings.distantSlices.Get(), 16u, 160u);
	settings.historyMissSamples = std::clamp(settings.historyMissSamples, 1u, 16u);
	settings.nearRange = bounded(settings.nearRange, defaults.nearRange, 15.0f / Util::Units::GAME_UNIT_TO_M, 700.0f / Util::Units::GAME_UNIT_TO_M);
	settings.localRange = bounded(settings.localRange, defaults.localRange, std::max(30.0f / Util::Units::GAME_UNIT_TO_M, settings.nearRange + 1000.0f), 3000.0f / Util::Units::GAME_UNIT_TO_M);
	settings.historyWeight = bounded(settings.historyWeight, defaults.historyWeight, 0.0f, 0.99f);
	settings.sampleJitter = bounded(settings.sampleJitter, defaults.sampleJitter, 0.0f, 1.0f);
	settings.shadowBias = bounded(settings.shadowBias, defaults.shadowBias, 0.0f, 0.05f);
	settings.depthDistribution = bounded(settings.depthDistribution, defaults.depthDistribution, 1.0f, 128.0f);
	settings.lookupJitter = bounded(settings.lookupJitter, defaults.lookupJitter, 0.0f, 1.0f);
	lastFrame = UINT32_MAX;
}

void ViewAtmosphere::SaveSettings(json& value)
{
	value = settings;
}

void ViewAtmosphere::RestoreDefaultSettings()
{
	settings = {};
	lastFrame = UINT32_MAX;
}

std::pair<std::string, std::vector<std::string>> ViewAtmosphere::GetFeatureSummary()
{
	return { T(TKEY("description"), "Shared rendering quality and composition for Physical Sky and Exponential Height Fog."),
		{ T(TKEY("quality_summary"), "One global set of grid and sampling controls"),
			T(TKEY("composition_summary"), "Atmosphere, fog and clouds composed after water") } };
}

void ViewAtmosphere::DrawSettings()
{
	ImGui::TextWrapped("%s", T(TKEY("ownership_hint"), "These settings control rendering quality, not the weather. Configure atmosphere and cloud appearance in Physical Sky, and fog density and lighting in Exponential Height Fog."));
	if (Util::Checkbox(T(TKEY("shared_volume"), "Use Shared Atmosphere and Fog Volume"), settings.enabled))
		lastFrame = UINT32_MAX;
	const bool enabled = settings.enabled.Get() != 0;
	ImGui::TextWrapped("%s", T(TKEY("shared_hint"), "The shared volume requires active Physical Sky. Otherwise aerial perspective uses its LUT and volumetric fog uses separate volumes. Quality settings below also apply to standalone volumetric fog. Reflections use a separate low-resolution atmosphere and fog cache when the shared volume is enabled."));
	ImGui::TextUnformatted(IsReady() ? T(TKEY("status_shared"), "Active: shared atmosphere and fog volume") : T(TKEY("status_separate"), "Active: separate rendering paths"));

	const int minPixels = 4, maxPixels = 64, minSlices = 16, maxSlices = 160;
	const auto distanceSlider = [](const char* label, float& value, float minimum, float maximum) {
		float meters = value * Util::Units::GAME_UNIT_TO_M;
		if (ImGui::SliderFloat(label, &meters, minimum, maximum, "%.1f m", ImGuiSliderFlags_AlwaysClamp))
			value = meters / Util::Units::GAME_UNIT_TO_M;
	};
	ImGui::SeparatorText(T(TKEY("grid_quality"), "Grid Quality"));
	ImGui::TextWrapped("%s", T(TKEY("grid_hint"), "Smaller pixel sizes and more depth slices improve detail at a higher cost. The shared volume continues beyond the local range to 40 km; standalone volumetric fog ends at the local range."));
	distanceSlider(T(TKEY("near_range"), "Near Grid End"), settings.nearRange, 15.0f, std::min(700.0f, (settings.localRange - 1000.0f) * Util::Units::GAME_UNIT_TO_M));
	distanceSlider(T(TKEY("local_range"), "Local Volume End"), settings.localRange, std::max(30.0f, (settings.nearRange + 1000.0f) * Util::Units::GAME_UNIT_TO_M), 3000.0f);
	if (Util::SliderInt(T(TKEY("near_pixels"), "Near Grid Pixel Size"), settings.nearPixelSize, minPixels, maxPixels, "%d", ImGuiSliderFlags_AlwaysClamp)) {
		settings.middlePixelSize = std::max(settings.middlePixelSize.Get(), settings.nearPixelSize.Get());
		settings.distantPixelSize = std::max(settings.distantPixelSize.Get(), settings.nearPixelSize.Get());
	}
	// The later grids are never finer than the near grid.
	const uint32_t nearPixels = settings.nearPixelSize;
	Util::SliderInt(T(TKEY("near_slices"), "Near Grid Depth Slices"), settings.nearSlices, minSlices, maxSlices, "%d", ImGuiSliderFlags_AlwaysClamp);
	Util::SliderInt(T(TKEY("middle_pixels"), "Middle Grid Pixel Size"), settings.middlePixelSize, static_cast<int>(nearPixels), maxPixels, "%d", ImGuiSliderFlags_AlwaysClamp);
	Util::SliderInt(T(TKEY("middle_slices"), "Middle Grid Depth Slices"), settings.middleSlices, minSlices, maxSlices, "%d", ImGuiSliderFlags_AlwaysClamp);
	if (enabled && globals::features::physicalSky.loaded && globals::features::physicalSky.cbData.enabled) {
		Util::SliderInt(T(TKEY("distant_pixels"), "Distant Atmosphere Pixel Size"), settings.distantPixelSize, static_cast<int>(nearPixels), maxPixels, "%d", ImGuiSliderFlags_AlwaysClamp);
		Util::SliderInt(T(TKEY("distant_slices"), "Distant Atmosphere Depth Slices"), settings.distantSlices, minSlices, maxSlices, "%d", ImGuiSliderFlags_AlwaysClamp);
	}
	ImGui::SeparatorText(T(TKEY("sampling"), "Sampling"));
	ImGui::SliderFloat(T(TKEY("history_weight"), "Temporal History Weight"), &settings.historyWeight, 0.0f, 0.99f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("sample_jitter"), "Sample Jitter"), &settings.sampleJitter, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("shadow_bias"), "Directional Shadow Bias"), &settings.shadowBias, 0.0f, 0.05f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::TextWrapped("%s", T(TKEY("sampling_hint"), "More history reduces noise but reacts more slowly. Sample jitter moves lighting samples inside each cell. Excessive shadow bias can cause light leaks."));
	if (!IsReady() && ImGui::TreeNode(T(TKEY("standalone_sampling"), "Standalone Fog Sampling"))) {
		ImGui::TextWrapped("%s", T(TKEY("standalone_hint"), "These controls only affect the separate volumetric fog path."));
		ImGui::SliderFloat(T(TKEY("depth_distribution"), "Depth Distribution Scale"), &settings.depthDistribution, 1.0f, 128.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		const uint32_t minMiss = 1, maxMiss = 16;
		ImGui::SliderScalar(T(TKEY("history_miss_samples"), "History Miss Samples"), ImGuiDataType_U32, &settings.historyMissSamples, &minMiss, &maxMiss, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("lookup_jitter"), "Lookup Jitter"), &settings.lookupJitter, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::TreePop();
	}
}

void ViewAtmosphere::DrawDebugSection()
{
	Util::DebugSection section;
	if (!section)
		return;

	preview.Draw(Luminance(), "luminance", T(TKEY("debug_luminance"), "Integrated Luminance"), 1.0f);
	preview.Draw(Transmittance(), "transmittance", T(TKEY("debug_transmittance"), "RGB Transmittance"), 1.0f);
	preview.Draw(Source(), "source", T(TKEY("debug_source"), "Local Light Source / Fog Extinction"), 1.0f);
}

#undef I18N_KEY_PREFIX

void ViewAtmosphere::SetupResources()
{
	ORGViewAtmosphere::Get().Setup();
	ORGViewAtmosphereReflection::Get().Setup();
}

ViewAtmosphere::AtlasLayout ViewAtmosphere::LayoutFor(float2 a_screen, std::uint32_t a_nearPixels, std::uint32_t a_middlePixels, std::uint32_t a_distantPixels,
	std::uint32_t a_nearSlices, std::uint32_t a_middleSlices, std::uint32_t a_distantSlices)
{
	// The later grids are never finer than the near grid.
	const uint32_t pixelSize = std::clamp(a_nearPixels, 4u, 64u);
	const uint32_t sizes[3] = { pixelSize, std::max(pixelSize, std::clamp(a_middlePixels, 4u, 64u)), std::max(pixelSize, std::clamp(a_distantPixels, 4u, 64u)) };
	const uint32_t layers[3] = { std::clamp(a_nearSlices, 16u, 160u), std::clamp(a_middleSlices, 16u, 160u), std::clamp(a_distantSlices, 16u, 160u) };
	AtlasLayout layout{};
	uint32_t gridScale[3] = { 1u, 1u, 1u };
	// While the atlas is over 8 M texels, the largest grid gets coarser.
	for (;;) {
		for (uint32_t i = 0; i < 3; ++i)
			layout.size[i] = { std::max(1u, uint32_t(std::ceil(a_screen.x / (sizes[i] * gridScale[i])))),
				std::max(1u, uint32_t(std::ceil(a_screen.y / (sizes[i] * gridScale[i])))), layers[i], 0u };
		layout.offset[0] = { 0, 0, 0, 0 };
		layout.offset[1] = { layout.size[2].x, layout.size[0].y, 0, 0 };
		layout.offset[2] = { 0, layout.size[0].y, 0, 0 };
		layout.atlas = { std::max(layout.size[0].x, layout.size[1].x + layout.size[2].x), layout.size[0].y + std::max(layout.size[1].y, layout.size[2].y),
			std::max({ layers[0], layers[1], layers[2] }) + 1u, 0 };
		if (uint64_t(layout.atlas.x) * layout.atlas.y * layout.atlas.z <= 8ull * 1024ull * 1024ull)
			return layout;
		uint32_t largestGrid = 0;
		uint64_t largestSize = 0;
		for (uint32_t i = 0; i < 3; ++i) {
			const auto size = layout.size[i];
			const uint64_t volume = uint64_t(size.x) * size.y * (size.z + 1u);
			if (volume > largestSize) {
				largestGrid = i;
				largestSize = volume;
			}
		}
		++gridScale[largestGrid];
	}
}

D3D11_SAMPLER_DESC ViewAtmosphere::SamplerDesc(bool a_comparison)
{
	D3D11_SAMPLER_DESC desc{};
	desc.Filter = a_comparison ? D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	desc.MaxLOD = D3D11_FLOAT32_MAX;
	if (a_comparison)
		desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
	return desc;
}

std::uint32_t ViewAtmosphere::InputFlags::Bits() const
{
	return (directionalShadow ? 1u : 0u) | (ibl ? 4u : 0u) | (probes ? 8u : 0u) | (localLights ? 32u : 0u) | (localShadows ? 64u : 0u) |
	       (engineShadows ? 128u : 0u) | (terrain ? 256u : 0u) | (cloudLayer ? 512u : 0u) | (environment ? 1024u : 0u);
}

ViewAtmosphere::Parameters ViewAtmosphere::ParametersFor(const AtlasLayout& a_layout, bool a_useFog, bool a_continuous) const
{
	const float maxDistance = 40000.0f / Util::Units::GAME_UNIT_TO_M;
	const float nearEnd = std::clamp(settings.nearRange, 1000.0f, maxDistance * 0.1f);
	const float middleEnd = std::clamp(settings.localRange, nearEnd + 1000.0f, maxDistance * 0.5f);
	const float ends[4] = { 0.0f, nearEnd, middleEnd, maxDistance };
	Parameters next{};
	for (uint32_t i = 0; i < 3; ++i) {
		auto& grid = next.grids[i];
		grid.size = a_layout.size[i];
		grid.offset = a_layout.offset[i];
		const float scale = i == 0 ? 10.0f / Util::Units::GAME_UNIT_TO_M : ends[i];
		grid.depth = { ends[i], ends[i + 1], scale, std::log2(1.0f + (ends[i + 1] - ends[i]) / scale) };
	}
	next.atlas = a_layout.atlas;
	next.flags = { 1u, uint32_t(a_useFog), 0u, 0u };
	const bool temporal = Util::GetTemporal();
	const float historyWeight = std::clamp(settings.historyWeight, 0.0f, 0.99f);
	next.history = { a_continuous && temporal ? historyWeight : 0.0f,
		temporal && historyWeight > 0.0f ? 1.0f : 0.0f,
		temporal && historyWeight > 0.0f ? std::clamp(settings.sampleJitter, 0.0f, 1.0f) : 0.0f, 0 };
	next.clipToWorld = globals::game::frameBufferCached.GetCameraViewProjUnjittered().Invert();
	return next;
}

bool ViewAtmosphere::Prepare()
{
	prepared = ready = false;
	// The graph modes' medium is ORGViewAtmosphere's. Nothing here runs D3D11 work there. The shared volume replaces the fog's
	// volumes and the AP shadow while the adopted revision runs it, as a prepared volume does here.
	if (ORGViewAtmosphere::On()) {
		auto& graph = ORGViewAtmosphere::Get();
		graph.Prepass();
		lastFrame = UINT32_MAX;
		prepared = ready = graph.SharedVolumeActive();
		return prepared;
	}
	Unbind();
	auto& sky = globals::features::physicalSky;
	if (!sky.loaded || !sky.cbData.enabled || !loaded || !settings.enabled || !sky.texTrLut || !sky.texMsLut || !sky.texSkyTail || !sky.csSkyTailGen ||
		globals::state->isMapMenuOpen || !CompositeEnabled()) {
		lastFrame = UINT32_MAX;
		return false;
	}
	if (!injectCS || !integrateCS) {
		std::vector<std::pair<const char*, const char*>> defines{ { "PHYSICAL_SKY", "" } };
		if (globals::features::exponentialHeightFog.loaded)
			defines.emplace_back("EXP_HEIGHT_FOG", "");
		if (globals::features::lightLimitFix.loaded)
			defines.emplace_back("LIGHT_LIMIT_FIX", "");
		if (globals::features::terrainShadows.loaded)
			defines.emplace_back("TERRAIN_SHADOWS", "");
		if (globals::features::cloudShadows.loaded)
			defines.emplace_back("CLOUD_SHADOWS", "");
		if (!injectCS)
			injectCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ViewAtmosphere\\LightScattering.cs.hlsl", defines, "cs_5_0")));
		if (!integrateCS)
			integrateCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ViewAtmosphere\\Integrate.cs.hlsl", { { "PHYSICAL_SKY", "" } }, "cs_5_0")));
		if (!injectCS || !integrateCS)
			return false;
	}
	if (!parameters) {
		parameters = std::make_unique<StructuredBuffer>(StructuredBufferDesc<Parameters>(uint64_t(1), false, true), 1u, "ViewAtmosphere::Parameters");
		parameters->CreateSRV();
		dispatchCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<DirectX::XMUINT4>(), "ViewAtmosphere::Dispatch");
		auto desc = SamplerDesc(false);
		if (!linearSampler)
			DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&desc, linearSampler.put()));
		Util::SetResourceName(linearSampler.get(), "ViewAtmosphere::LinearSampler");
		desc = SamplerDesc(true);
		DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&desc, shadowSampler.put()));
		Util::SetResourceName(shadowSampler.get(), "ViewAtmosphere::ShadowSampler");
	}
	auto& fog = globals::features::exponentialHeightFog;
	const bool useFog = fog.IsActive();
	const float2 screen = Util::ConvertToDynamic(float2{ float(globals::game::graphicsState->screenWidth), float(globals::game::graphicsState->screenHeight) });
	const auto layout = LayoutFor(screen, settings.nearPixelSize, settings.middlePixelSize, settings.distantPixelSize, settings.nearSlices, settings.middleSlices,
		settings.distantSlices);
	Parameters next = ParametersFor(layout, useFog, false);
	const bool resized = !source || std::memcmp(&data.atlas, &next.atlas, sizeof(next.atlas)) != 0;
	const bool changed = resized || std::memcmp(data.grids, next.grids, sizeof(next.grids)) != 0 || data.flags.y != uint32_t(useFog);
	if (resized) {
		auto makeVolume = [&](const char* name) {
			D3D11_TEXTURE3D_DESC desc{};
			desc.Width = next.atlas.x;
			desc.Height = next.atlas.y;
			desc.Depth = next.atlas.z;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			auto texture = std::make_unique<Texture3D>(desc, name);
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = desc.Format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
			srv.Texture3D.MipLevels = 1;
			texture->CreateSRV(srv);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = desc.Format;
			uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
			uav.Texture3D.WSize = desc.Depth;
			texture->CreateUAV(uav);
			return texture;
		};
		source = makeVolume("ViewAtmosphere::Source");
		history = makeVolume("ViewAtmosphere::History");
		integratedLum = makeVolume("ViewAtmosphere::Luminance");
		integratedTr = makeVolume("ViewAtmosphere::Transmittance");
		const float zero[4]{};
		const float one[4]{ 1, 1, 1, 1 };
		globals::d3d::context->ClearUnorderedAccessViewFloat(source->uav.get(), zero);
		globals::d3d::context->ClearUnorderedAccessViewFloat(history->uav.get(), zero);
		globals::d3d::context->ClearUnorderedAccessViewFloat(integratedLum->uav.get(), zero);
		globals::d3d::context->ClearUnorderedAccessViewFloat(integratedTr->uav.get(), one);
	}
	next.flags.z = data.flags.z;
	next.history = ParametersFor(layout, useFog, !changed && lastFrame != UINT32_MAX && lastFrame + 1u == globals::state->frameCount).history;
	data = next;
	prepared = true;
	return true;
}

void ViewAtmosphere::Render()
{
	// The graph modes run the volume at the Prepass anchor (ORGViewAtmosphere).
	if (!prepared || ORGViewAtmosphere::On())
		return;
	auto* context = globals::d3d::context;
	auto& sky = globals::features::physicalSky;
	auto& fog = globals::features::exponentialHeightFog;
	auto& terrain = globals::features::terrainShadows;
	auto& clouds = globals::features::cloudShadows;
	auto& ibl = globals::features::ibl;
	auto& skylighting = globals::features::skylighting;
	auto& lights = globals::features::lightLimitFix;
	auto& cubemaps = globals::features::dynamicCubemaps;
	auto* environment = cubemaps.loaded && cubemaps.cubemapValid[0] && cubemaps.envTextureBC6H ? cubemaps.envTextureBC6H->srv.get() : nullptr;
	const bool localLights = data.flags.y && fog.settings.volumetricFogEnabled && lights.loaded && lights.lights && lights.lightGrid && lights.lightIndexList;
	const bool localShadows = localLights && lights.IsLocalShadowCacheActive() && lights.localShadowBuffer && lights.localShadowCache;
	auto [engineShadows, engineShadowCB] = PrepareLocalShadows(localLights);
	const bool useIBL = data.flags.y && ibl.loaded && ibl.settings.EnableIBL && !ibl.IsDisabledForCurrentScene() && ibl.envIBLTexture && ibl.skyIBLTexture;
	auto* shadowView = globals::deferred->GetDirectionalShadowMap();
	auto* shadowData = globals::deferred->directionalShadowLights ? globals::deferred->directionalShadowLights->srv.get() : nullptr;
	const bool terrainReady = terrain.IsShadowMapReady();
	auto* cloudLayer = clouds.loaded ? clouds.texCloudShadowLayers[CloudShadows::kMaxCloudLayers - 1] : nullptr;
	const auto previousShadowFlags = data.flags.z & kShadowInputBits;
	// The graph's volumes while its probe update runs.
	auto* probes = skylighting.loaded ? skylighting.ProbeArraySRV() : nullptr;
	data.flags.z = InputFlags{ .directionalShadow = shadowView && shadowData,
		.ibl = useIBL,
		.probes = probes != nullptr,
		.localLights = localLights,
		.localShadows = localShadows,
		.engineShadows = engineShadows != nullptr,
		.terrain = terrainReady,
		.cloudLayer = cloudLayer != nullptr,
		.environment = environment != nullptr }
	                   .Bits();
	if ((data.flags.z & kShadowInputBits) != previousShadowFlags)
		data.history.x = 0.0f;
	parameters->Update(&data, sizeof(data));
	auto* paramSrv = parameters->SRV();
	context->CSSetShaderResources(127, 1, &paramSrv);
	ID3D11Buffer* shared[] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, shared);
	auto* frame = *globals::game::perFrame;
	context->CSSetConstantBuffers(12, 1, &frame);
	auto* dispatch = dispatchCB->CB();
	context->CSSetConstantBuffers(0, 1, &dispatch);
	context->CSSetConstantBuffers(1, 1, &engineShadowCB);
	ID3D11SamplerState* samplers[] = { linearSampler.get(), shadowSampler.get() };
	context->CSSetSamplers(0, 2, samplers);
	auto bind = [&](UINT slot, ID3D11ShaderResourceView* srv) { context->CSSetShaderResources(slot, 1, &srv); };
	bind(0, history->srv.get());
	bind(1, shadowView);
	bind(2, sky.texMsLut->srv.get());
	bind(5, engineShadows);
	bind(25, cloudLayer ? cloudLayer->srv.get() : nullptr);
	bind(31, environment);
	bind(35, localLights ? lights.lights->srv.get() : nullptr);
	bind(36, localLights ? lights.lightIndexList->srv.get() : nullptr);
	bind(37, localLights ? lights.lightGrid->srv.get() : nullptr);
	bind(50, probes);
	bind(60, terrainReady ? terrain.texShadowHeight->srv.get() : nullptr);
	bind(61, sky.texTrLut->srv.get());
	bind(76, useIBL ? ibl.envIBLTexture->srv.get() : nullptr);
	bind(77, useIBL ? ibl.skyIBLTexture->srv.get() : nullptr);
	bind(98, shadowData);
	bind(102, localShadows ? lights.localShadowBuffer->srv.get() : nullptr);
	bind(103, localShadows ? lights.localShadowCache->srv.get() : nullptr);
	bind(112, sky.texShadowVolume ? sky.texShadowVolume->srv.get() : nullptr);
	bind(69, sky.texCloudShadowMean ? sky.texCloudShadowMean->srv.get() : nullptr);
	bind(70, CloudDiffuseTransport());
	ID3D11UnorderedAccessView* nullUavs[2]{};
	globals::profiler->BeginPass("ViewAtmosphere::LightScattering");
	for (uint32_t level = 0; level < 3; ++level) {
		dispatchCB->Update(DirectX::XMUINT4{ level, 0, 0, 0 });
		auto* uav = source->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(injectCS.get(), nullptr, 0);
		const auto size = data.grids[level].size;
		context->Dispatch((size.x + 7u) / 8u, (size.y + 7u) / 8u, (size.z + 3u) / 4u);
		context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	}
	globals::profiler->EndPass();
	bind(0, source->srv.get());
	globals::profiler->BeginPass("ViewAtmosphere::Integration");
	for (uint32_t level = 0; level < 3; ++level) {
		dispatchCB->Update(DirectX::XMUINT4{ level, 0, 0, 0 });
		ID3D11UnorderedAccessView* uavs[] = { integratedLum->uav.get(), integratedTr->uav.get() };
		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
		context->CSSetShader(integrateCS.get(), nullptr, 0);
		const auto size = data.grids[level].size;
		context->Dispatch((size.x + 7u) / 8u, (size.y + 7u) / 8u, 1);
		context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	}
	globals::profiler->EndPass();
	for (UINT slot : { 0u, 1u, 2u, 5u, 25u, 31u, 35u, 36u, 37u, 50u, 60u, 61u, 67u, 68u, 69u, 70u, 76u, 77u, 98u, 102u, 103u, 112u, 127u })
		bind(slot, nullptr);
	ID3D11Buffer* nullBuffers[2]{};
	ID3D11SamplerState* nullSamplers[2]{};
	context->CSSetConstantBuffers(0, 2, nullBuffers);
	context->CSSetSamplers(0, 2, nullSamplers);
	context->CSSetShader(nullptr, nullptr, 0);
	source.swap(history);
	lastFrame = globals::state->frameCount;
	ready = true;
	Bind();
	Bind(true);
}

void ViewAtmosphere::Bind(bool compute)
{
	// The graph's readers take the medium by its published ids.
	if (!ready || ORGViewAtmosphere::On())
		return;
	ID3D11ShaderResourceView* srvs[] = { integratedLum->srv.get(), integratedTr->srv.get(), parameters->SRV() };
	if (compute)
		globals::d3d::context->CSSetShaderResources(125, 3, srvs);
	else
		globals::d3d::context->PSSetShaderResources(125, 3, srvs);
}

std::array<std::uint32_t, 64> ViewAtmosphere::ReflectionParameters(const ReflectionFlags& a_flags)
{
	Parameters reflection{};
	reflection.grids[0].size = { 32, 32, 32, 0 };
	const float offset = 10.0f / Util::Units::GAME_UNIT_TO_M;
	const float distance = 40000.0f / Util::Units::GAME_UNIT_TO_M;
	reflection.grids[0].depth = { 0, distance, offset, std::log2(1.0f + distance / offset) };
	reflection.atlas = { 32, 32, 32, 0 };
	reflection.flags = { 1u, uint32_t(a_flags.fog),
		(a_flags.ibl ? 4u : 0u) | (a_flags.probes ? 8u : 0u) | (a_flags.terrain ? 256u : 0u) | (a_flags.cloudLayer ? 512u : 0u), 1u };
	std::array<std::uint32_t, 64> words{};
	static_assert(sizeof(words) == sizeof(reflection));
	std::memcpy(words.data(), &reflection, sizeof(reflection));
	return words;
}

void ViewAtmosphere::RenderReflection()
{
	// The graph's passes run at the ReflectionsPrepass anchor, which comes before this. Here they note what they read.
	if (ORGViewAtmosphere::On()) {
		ORGViewAtmosphereReflection::Get().Observe();
		return;
	}
	Unbind();
	auto& sky = globals::features::physicalSky;
	auto& fog = globals::features::exponentialHeightFog;
	if (!loaded || !settings.enabled || !ready || !sky.loaded || !sky.cbData.enabled || globals::state->isMapMenuOpen ||
		!sky.texTrLut || !sky.texMsLut || !sky.texSvLut || !sky.texApLut || !sky.texSkyTail || !sky.csSkyTailGen || !CompositeEnabled())
		return;
	if (!reflectionShadersRequested) {
		reflectionShadersRequested = true;
		std::vector<std::pair<const char*, const char*>> defines{ { "PHYSICAL_SKY", "" } };
		if (fog.loaded)
			defines.emplace_back("EXP_HEIGHT_FOG", "");
		if (globals::features::terrainShadows.loaded)
			defines.emplace_back("TERRAIN_SHADOWS", "");
		if (globals::features::cloudShadows.loaded)
			defines.emplace_back("CLOUD_SHADOWS", "");
		reflectionCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ViewAtmosphere\\Reflection.cs.hlsl", defines, "cs_5_0")));
		defines.emplace_back("REFLECTION_SKY_SH", "");
		reflectionSkyCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ViewAtmosphere\\Reflection.cs.hlsl", defines, "cs_5_0")));
	}
	if (!reflectionCS || !reflectionSkyCS)
		return;
	auto* context = globals::d3d::context;
	if (!reflectionParameters) {
		reflectionParameters = std::make_unique<StructuredBuffer>(StructuredBufferDesc<Parameters>(uint64_t(1), false, true), 1u, "ViewAtmosphere::ReflectionParameters");
		reflectionParameters->CreateSRV();
		auto makeVolume = [](const char* name) {
			D3D11_TEXTURE3D_DESC desc{};
			desc.Width = desc.Height = desc.Depth = 32;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			auto texture = std::make_unique<Texture3D>(desc, name);
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = desc.Format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
			srv.Texture3D.MipLevels = 1;
			texture->CreateSRV(srv);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = desc.Format;
			uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
			uav.Texture3D.WSize = desc.Depth;
			texture->CreateUAV(uav);
			return texture;
		};
		reflectionLum = makeVolume("ViewAtmosphere::ReflectionLuminance");
		reflectionTr = makeVolume("ViewAtmosphere::ReflectionTransmittance");
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = 3;
		desc.Height = 2;  // Sky SH plus the cloud/clear-sky diffuse transport ratio.
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		reflectionSkySH = std::make_unique<Texture2D>(desc, "ViewAtmosphere::ReflectionSkySH");
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = desc.Format;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		reflectionSkySH->CreateSRV(srv);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = desc.Format;
		uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		reflectionSkySH->CreateUAV(uav);
		D3D11_SAMPLER_DESC sampler{};
		sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&sampler, reflectionSampler.put()));
		Util::SetResourceName(reflectionSampler.get(), "ViewAtmosphere::ReflectionSampler");
	}
	// One update before all six faces; reuse the completed sky/cloud LUTs, including
	// SkyTail. Rebuilding its 200x150x30 ray march here would dominate this small cache.
	auto& terrain = globals::features::terrainShadows;
	auto& clouds = globals::features::cloudShadows;
	auto& skylighting = globals::features::skylighting;
	auto& ibl = globals::features::ibl;
	const bool useFog = fog.IsActive();
	const bool useIBL = useFog && ibl.loaded && ibl.settings.EnableIBL && !ibl.IsDisabledForCurrentScene() && ibl.envIBLTexture;
	const bool terrainReady = terrain.IsShadowMapReady();
	auto* cloudLayer = clouds.loaded ? clouds.texCloudShadowLayers[CloudShadows::kMaxCloudLayers - 1] : nullptr;
	auto* probes = skylighting.loaded ? skylighting.ProbeArraySRV() : nullptr;
	const auto reflection = ReflectionParameters({ .fog = useFog, .ibl = useIBL, .probes = probes != nullptr, .terrain = terrainReady, .cloudLayer = cloudLayer != nullptr });
	reflectionParameters->Update(reflection.data(), sizeof(reflection));
	auto bind = [&](UINT slot, ID3D11ShaderResourceView* srv) { context->CSSetShaderResources(slot, 1, &srv); };
	ID3D11Buffer* shared[] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, shared);
	auto* frame = *globals::game::perFrame;
	context->CSSetConstantBuffers(12, 1, &frame);
	auto* sampler = reflectionSampler.get();
	context->CSSetSamplers(0, 1, &sampler);
	bind(2, sky.texMsLut->srv.get());
	bind(25, cloudLayer ? cloudLayer->srv.get() : nullptr);
	bind(50, probes);
	bind(60, terrainReady ? terrain.texShadowHeight->srv.get() : nullptr);
	bind(61, sky.texTrLut->srv.get());
	bind(62, sky.texSvLut->srv.get());
	bind(63, sky.texApLut->srv.get());
	bind(67, sky.texVolCubeAux ? sky.texVolCubeAux->srv.get() : nullptr);
	bind(76, useIBL ? ibl.envIBLTexture->srv.get() : nullptr);
	bind(77, nullptr);
	bind(112, sky.texShadowVolume ? sky.texShadowVolume->srv.get() : nullptr);
	bind(69, sky.texCloudShadowMean ? sky.texCloudShadowMean->srv.get() : nullptr);
	bind(114, sky.texVolCubeTr ? sky.texVolCubeTr->srv.get() : nullptr);
	bind(115, sky.texVolCubeLum ? sky.texVolCubeLum->srv.get() : nullptr);
	bind(127, reflectionParameters->SRV());
	ID3D11UnorderedAccessView* outputs[] = { reflectionSkySH->uav.get(), nullptr };
	ID3D11UnorderedAccessView* nullOutputs[2]{};
	bind(70, nullptr);
	globals::profiler->BeginPass("ViewAtmosphere::ReflectionSkySH");
	context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
	context->CSSetShader(reflectionSkyCS.get(), nullptr, 0);
	context->Dispatch(1, 1, 1);
	context->CSSetUnorderedAccessViews(0, 2, nullOutputs, nullptr);
	globals::profiler->EndPass();
	bind(77, reflectionSkySH->srv.get());
	bind(70, CloudDiffuseTransport());
	outputs[0] = reflectionLum->uav.get();
	outputs[1] = reflectionTr->uav.get();
	globals::profiler->BeginPass("ViewAtmosphere::ReflectionMedium");
	context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
	context->CSSetShader(reflectionCS.get(), nullptr, 0);
	context->Dispatch(16, 32, 1);
	context->CSSetUnorderedAccessViews(0, 2, nullOutputs, nullptr);
	globals::profiler->EndPass();
	for (UINT slot : { 2u, 25u, 50u, 60u, 61u, 62u, 63u, 67u, 69u, 70u, 76u, 77u, 112u, 114u, 115u, 127u })
		bind(slot, nullptr);
	sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);
	context->CSSetShader(nullptr, nullptr, 0);
	ID3D11ShaderResourceView* result[] = { reflectionLum->srv.get(), reflectionTr->srv.get(), reflectionParameters->SRV() };
	context->PSSetShaderResources(125, 3, result);
	auto* tail = sky.texSkyTail->srv.get();
	context->PSSetShaderResources(66, 1, &tail);
}

void ViewAtmosphere::Unbind()
{
	if (ORGViewAtmosphere::On())
		return;
	ID3D11ShaderResourceView* nullSrvs[3]{};
	globals::d3d::context->PSSetShaderResources(125, 3, nullSrvs);
	globals::d3d::context->CSSetShaderResources(125, 3, nullSrvs);
}

void ViewAtmosphere::ClearShaderCache()
{
	ORGViewAtmosphere::Get().ClearShaderCache();
	preview.ClearShaderCache();
	mediumCompositeCS = nullptr;
	mediumShaderRequested = false;
	reflectionCS = nullptr;
	reflectionSkyCS = nullptr;
	reflectionShadersRequested = false;
	injectCS = nullptr;
	integrateCS = nullptr;
	prepared = ready = false;
	lastFrame = UINT32_MAX;
	Unbind();
}

bool ViewAtmosphere::CompositeEnabled()
{
	if (ORGViewAtmosphere::On())
		return ORGViewAtmosphere::Get().CompositeActive();
	if (!loaded || !globals::deferred->postWaterHookInstalled || !globals::shaderCache->IsEnabled() ||
		!globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].UAV)
		return false;
	const auto& sky = globals::features::physicalSky;
	const auto& fog = globals::features::exponentialHeightFog;
	if (!(sky.loaded && sky.cbData.enabled) && !(fog.loaded && fog.settings.enabled))
		return false;
	if (!mediumShaderRequested) {
		mediumShaderRequested = true;
		std::vector<std::pair<const char*, const char*>> defines;
		if (sky.loaded)
			defines.emplace_back("PHYSICAL_SKY", "");
		if (fog.loaded)
			defines.emplace_back("EXP_HEIGHT_FOG", "");
		if (globals::features::ibl.loaded)
			defines.emplace_back("IBL", "");
		if (globals::features::dynamicCubemaps.loaded)
			defines.emplace_back("DYNAMIC_CUBEMAPS", "");
		if (auto* shader = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ViewAtmosphere\\Composite.cs.hlsl", defines, "cs_5_0")))
			mediumCompositeCS.attach(shader);
	}
	if (mediumCompositeCS && !linearSampler) {
		D3D11_SAMPLER_DESC desc{};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&desc, linearSampler.put()));
		Util::SetResourceName(linearSampler.get(), "ViewAtmosphere::LinearSampler");
	}
	return mediumCompositeCS != nullptr;
}

void ViewAtmosphere::CompositeAfterWater()
{
	if (ORGViewAtmosphere::On()) {
		ORGViewAtmosphere::Get().CompositeAfterWater();
		return;
	}
	if (!CompositeEnabled() ||
		!globals::state->inWorld ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)) != 0 ||
		mediumCompositeFrame == globals::state->frameCount)
		return;
	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!main.UAV)
		return;
	context->OMSetRenderTargets(0, nullptr, nullptr);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	ID3D11Buffer* perFrame = *globals::game::perFrame;
	context->CSSetConstantBuffers(12, 1, &perFrame);
	ID3D11Buffer* sharedBuffers[] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, sharedBuffers);
	auto& sky = globals::features::physicalSky;
	if (sky.loaded && sky.cbData.enabled && sky.cbData.enableVolumetricClouds)
		sky.RenderVolumetricClouds(PhysicalSky::VolumetricCloudPass::kMainViewAndCubemap);

	const std::array<uint32_t, 7> inheritedSlots = { 19, 22, 31, 76, 77, 78, 79 };
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 7> inherited;
	for (size_t i = 0; i < inheritedSlots.size(); ++i) {
		context->PSGetShaderResources(inheritedSlots[i], 1, inherited[i].put());
		auto* srv = inherited[i].get();
		context->CSSetShaderResources(inheritedSlots[i], 1, &srv);
	}
	std::array<ID3D11ShaderResourceView*, 4> atmosphere = {
		sky.texTrLut ? sky.texTrLut->srv.get() : nullptr, sky.texSvLut ? sky.texSvLut->srv.get() : nullptr, sky.texApLut ? sky.texApLut->srv.get() : nullptr, sky.texApShadow ? sky.texApShadow->srv.get() : nullptr
	};
	context->CSSetShaderResources(61, 4, atmosphere.data());
	Bind(true);
	std::array<ID3D11ShaderResourceView*, 3> clouds = {
		sky.texVolFilteredTr ? sky.texVolFilteredTr->srv.get() : nullptr,
		sky.texVolFilteredLum ? sky.texVolFilteredLum->srv.get() : nullptr,
		sky.texShadowVolume ? sky.texShadowVolume->srv.get() : nullptr
	};
	context->CSSetShaderResources(110, 3, clouds.data());
	auto* lightningChannels = sky.texLightningChannels ? sky.texLightningChannels->srv.get() : nullptr;
	context->CSSetShaderResources(113, 1, &lightningChannels);
	// Engine target fields can be tripwire handles: DXVK takes the wrapped object.
	using RenderGraph::Engine::Tripwire::Unwrap;
	std::array<ID3D11ShaderResourceView*, 2> depths = {
		Unwrap(renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV),
		sky.texVolFilteredAux ? sky.texVolFilteredAux->srv.get() : nullptr
	};
	context->CSSetShaderResources(40, 2, depths.data());
	std::array<ID3D11SamplerState*, 5> samplers = { linearSampler.get(), nullptr, nullptr, linearSampler.get(), linearSampler.get() };
	context->CSSetSamplers(0, 5, samplers.data());
	auto* mainUav = Unwrap(main.UAV);
	context->CSSetUnorderedAccessViews(0, 1, &mainUav, nullptr);
	context->CSSetShader(mediumCompositeCS.get(), nullptr, 0);
	D3D11_TEXTURE2D_DESC desc;
	main.texture->GetDesc(&desc);
	const auto scale = globals::game::frameBufferCached.GetDynamicResolutionParams1();
	const uint32_t width = static_cast<uint32_t>(std::clamp(desc.Width * scale.x, 1.f, float(desc.Width)));
	const uint32_t height = static_cast<uint32_t>(std::clamp(desc.Height * scale.y, 1.f, float(desc.Height)));
	globals::state->BeginPerfEvent("Medium Composite");
	globals::profiler->BeginPass("ViewAtmosphere::Composite");
	context->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
	globals::profiler->EndPass();
	globals::state->EndPerfEvent();
	ID3D11UnorderedAccessView* nullUav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	std::array<ID3D11ShaderResourceView*, 128> nullSrvs = {};
	context->CSSetShaderResources(0, static_cast<uint32_t>(nullSrvs.size()), nullSrvs.data());
	context->CSSetShader(nullptr, nullptr, 0);
	ID3D11Buffer* nullBuffer = nullptr;
	context->CSSetConstantBuffers(12, 1, &nullBuffer);
	std::array<ID3D11SamplerState*, 5> nullSamplers = {};
	context->CSSetSamplers(0, static_cast<uint32_t>(nullSamplers.size()), nullSamplers.data());
	if (sky.texVolFilteredAux) {
		auto* aux = sky.texVolFilteredAux->srv.get();
		context->PSSetShaderResources(116, 1, &aux);
	}
	mediumCompositeFrame = globals::state->frameCount;
}
std::pair<ID3D11ShaderResourceView*, ID3D11Buffer*> ViewAtmosphere::PrepareLocalShadows(bool enabled)
{
	if (!enabled)
		return {};
	// The same data the fog's graph path latches.
	ExponentialHeightFog::EngineLocalShadows shadows{};
	auto* view = globals::features::exponentialHeightFog.BuildEngineLocalShadowData(shadows);
	if (!view)
		return {};
	if (!engineLocalShadowCB)
		engineLocalShadowCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<decltype(shadows)>(), "ViewAtmosphere::EngineLocalShadows");
	engineLocalShadowCB->Update(shadows);
	// The engine's field can be a tripwire handle: DXVK takes the wrapped object.
	return { RenderGraph::Engine::Tripwire::Unwrap(view), engineLocalShadowCB->CB() };
}

#include "ExponentialHeightFog.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/FrameCamera.h"
#include "Snapshot/Publication.h"

#include "Deferred.h"
#include "Effects11.h"
#include "Effects11/SettingManager.h"
#include "Features/CloudShadows.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/LightSet.h"
#include "Features/ExponentialHeightFog/ORGExponentialHeightFog.h"
#include "Features/IBL.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/PhysicalSky.h"
#include "Features/Skylighting.h"
#include "Features/TerrainShadows.h"
#include "Features/ViewAtmosphere.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/GpuBlock.h"

#define I18N_KEY_PREFIX "feature.exp_height_fog."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ExponentialHeightFog::Settings,
	enabled,
	useDynamicCubemaps,
	startDistance,
	fogHeight,
	fogHeightFalloff,
	fogDensity,
	fogHeight2,
	fogHeightFalloff2,
	fogDensity2,
	directionalInscatteringMultiplier,
	directionalInscatteringAnisotropy,
	useSkyIBL,
	inscatteringTint,
	cubemapMipLevel,
	sunlightAttenuationAmount,
	respectVanillaFogFade,
	disableVanillaFog,
	fogInscatteringColor,
	originalFogColorAmount,
	originalFogColorOverride,
	volumetricFogEnabled,
	volumetricFogStartDistance,
	volumetricFogNearFadeInDistance,
	volumetricFogExtinctionScale,
	volumetricFogScatteringDistribution,
	volumetricFogAlbedo,
	volumetricFogEmissive,
	volumetricDirectionalScatteringIntensity,
	volumetricSkyLightingIntensity,
	volumetricFogNoiseScale,
	volumetricFogNoiseThreshold,
	volumetricFogNoiseVelocity,
	volumetricLocalLightScatteringIntensity)

namespace
{
	float Halton(uint32_t a_index, uint32_t a_base)
	{
		float result = 0.0f;
		float invBase = 1.0f / static_cast<float>(a_base);
		float fraction = invBase;
		while (a_index > 0) {
			result += static_cast<float>(a_index % a_base) * fraction;
			a_index /= a_base;
			fraction *= invBase;
		}
		return result;
	}
}

ExponentialHeightFog::ExponentialHeightFog()
{
	settings.enabled.Bind(CS::SettingRegistry::Declare("ExponentialHeightFog.enabled"));
	settings.volumetricFogEnabled.Bind(CS::SettingRegistry::Declare("ExponentialHeightFog.volumetricFogEnabled"));
	settings.useSkyIBL.Bind(CS::SettingRegistry::Declare("ExponentialHeightFog.useSkyIBL"));
	settings.useDynamicCubemaps.Bind(CS::SettingRegistry::Declare("ExponentialHeightFog.useDynamicCubemaps"));
	settings.fogDensity.Bind(CS::SettingRegistry::Declare("ExponentialHeightFog.fogDensity"));
	settings.fogDensity2.Bind(CS::SettingRegistry::Declare("ExponentialHeightFog.fogDensity2"));
	settings.volumetricFogExtinctionScale.Bind(CS::SettingRegistry::Declare("ExponentialHeightFog.volumetricFogExtinctionScale"));
}

void ExponentialHeightFog::RestoreDefaultSettings()
{
	settings = {};
}

void ExponentialHeightFog::LoadSettings(json& o_json)
{
	settings = o_json;
}

void ExponentialHeightFog::SaveSettings(json& o_json)
{
	o_json = settings;
}

namespace
{
	using FogCpu = ExponentialHeightFog::Settings;
	using FogGpu = ExponentialHeightFog::GpuSettings;

	// Every GPU field comes from the setting of the same name.
	using FogGpuBlock = CS::GpuBlock<FogGpu,
		CS::GpuField<&FogCpu::enabled, &FogGpu::enabled>,
		CS::GpuField<&FogCpu::useDynamicCubemaps, &FogGpu::useDynamicCubemaps>,
		CS::GpuField<&FogCpu::startDistance, &FogGpu::startDistance>,
		CS::GpuField<&FogCpu::fogHeight, &FogGpu::fogHeight>,
		CS::GpuField<&FogCpu::fogHeightFalloff, &FogGpu::fogHeightFalloff>,
		CS::GpuField<&FogCpu::fogDensity, &FogGpu::fogDensity>,
		CS::GpuField<&FogCpu::fogHeight2, &FogGpu::fogHeight2>,
		CS::GpuField<&FogCpu::fogHeightFalloff2, &FogGpu::fogHeightFalloff2>,
		CS::GpuField<&FogCpu::fogDensity2, &FogGpu::fogDensity2>,
		CS::GpuField<&FogCpu::directionalInscatteringMultiplier, &FogGpu::directionalInscatteringMultiplier>,
		CS::GpuField<&FogCpu::directionalInscatteringAnisotropy, &FogGpu::directionalInscatteringAnisotropy>,
		CS::GpuField<&FogCpu::useSkyIBL, &FogGpu::useSkyIBL>,
		CS::GpuColour<&FogCpu::inscatteringTint, &FogGpu::inscatteringTint>,
		CS::GpuField<&FogCpu::cubemapMipLevel, &FogGpu::cubemapMipLevel>,
		CS::GpuField<&FogCpu::sunlightAttenuationAmount, &FogGpu::sunlightAttenuationAmount>,
		CS::GpuField<&FogCpu::respectVanillaFogFade, &FogGpu::respectVanillaFogFade>,
		CS::GpuField<&FogCpu::disableVanillaFog, &FogGpu::disableVanillaFog>,
		CS::GpuColour<&FogCpu::fogInscatteringColor, &FogGpu::fogInscatteringColor>,
		CS::GpuField<&FogCpu::originalFogColorAmount, &FogGpu::originalFogColorAmount>,
		CS::GpuField<&FogCpu::volumetricFogEnabled, &FogGpu::volumetricFogEnabled>,
		CS::GpuField<&FogCpu::volumetricFogStartDistance, &FogGpu::volumetricFogStartDistance>,
		CS::GpuField<&FogCpu::volumetricFogNearFadeInDistance, &FogGpu::volumetricFogNearFadeInDistance>,
		CS::GpuColour<&FogCpu::volumetricFogAlbedo, &FogGpu::volumetricFogAlbedo>,
		CS::GpuColour<&FogCpu::volumetricFogEmissive, &FogGpu::volumetricFogEmissive>,
		CS::GpuField<&FogCpu::volumetricDirectionalScatteringIntensity, &FogGpu::volumetricDirectionalScatteringIntensity>,
		CS::GpuField<&FogCpu::volumetricSkyLightingIntensity, &FogGpu::volumetricSkyLightingIntensity>,
		CS::GpuField<&FogCpu::volumetricFogScatteringDistribution, &FogGpu::volumetricFogScatteringDistribution>,
		CS::GpuField<&FogCpu::volumetricFogExtinctionScale, &FogGpu::volumetricFogExtinctionScale>,
		CS::GpuField<&FogCpu::volumetricLocalLightScatteringIntensity, &FogGpu::volumetricLocalLightScatteringIntensity>,
		CS::GpuField<&FogCpu::volumetricFogNoiseScale, &FogGpu::volumetricFogNoiseScale>,
		CS::GpuField<&FogCpu::volumetricFogNoiseThreshold, &FogGpu::volumetricFogNoiseThreshold>,
		CS::GpuField<&FogCpu::volumetricFogNoiseVelocity, &FogGpu::volumetricFogNoiseVelocity>,
		CS::GpuColour<&FogCpu::originalFogColorOverride, &FogGpu::originalFogColorOverride>>;
}

ExponentialHeightFog::GpuSettings ExponentialHeightFog::GetCommonBufferData() const
{
	GpuSettings data = FogGpuBlock::Fill(settings, [](float* a_colour) { globals::features::linearLighting.SRGBToWorking(a_colour); });

	if (IsSuppressed())
		data.enabled = 0;
	// On the graph the shaders read the volumes only once a run wrote the bound versions, as the D3D11 readers get them.
	const auto& orgFog = ORGExponentialHeightFog::Get();
	if (orgFog.IsActive() && !orgFog.NearSRV())
		data.volumetricFogEnabled = 0;
	return data;
}

bool ExponentialHeightFog::IsSuppressed() const
{
	// The flat world map keeps its vanilla fog; the 3D map runs on the scene kept from before it opened
	return globals::features::effects11.OwnsFeature(E11Handoff::Feature::HeightFog) || globals::state->IsFlatWorldMapOpen();
}

void ExponentialHeightFog::DrawSettings()
{
	const bool managedByEffects11 = globals::features::effects11.OwnsFeature(E11Handoff::Feature::HeightFog);
	const Util::LockedSection enbLock(managedByEffects11,
		T("common.settings_managed_by_enb", "This setting is managed by Effects 11."));
	const Util::DefaultsWhileActive<Settings> shownDefaults(settings, managedByEffects11);

	Util::Checkbox(T(TKEY("enable_exp_height_fog"), "Enable Exponential Height Fog"), settings.enabled);
	ImGui::SliderFloat(T(TKEY("start_distance"), "Start Distance"), &settings.startDistance, 0.0f, 100000.0f, "%.1f");
	ImGui::SliderFloat(T(TKEY("fog_height"), "Fog Height"), &settings.fogHeight, -22000.0f, 22000.0f, "%.1f");
	ImGui::SliderFloat(T(TKEY("fog_height_falloff"), "Fog Height Falloff"), &settings.fogHeightFalloff, 0.001f, 2.0f, "%.3f");
	ImGui::ColorEdit4(T(TKEY("fog_inscattering_color"), "Fog Inscattering Color"), (float*)&settings.fogInscatteringColor);
	ImGui::SliderFloat(T(TKEY("original_fog_color_amount"), "Original Fog Color Amount"), &settings.originalFogColorAmount, 0.0f, 1.0f, "%.2f");
	ImGui::ColorEdit4(T(TKEY("original_fog_color_override"), "Original Fog Color Override"), (float*)&settings.originalFogColorOverride);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("original_fog_color_override_tooltip"),
							  "Replaces the weather's fog colour used by Original Fog Color Amount.\n"
							  "Alpha blends from the weather colour (0) to this colour (1)."));
	}
	Util::SliderFloat(T(TKEY("fog_density"), "Fog Density"), settings.fogDensity, 0.0f, 1.0f, "%.3f");
	if (ImGui::TreeNode(T(TKEY("second_fog_layer"), "Second Fog Layer"))) {
		ImGui::SliderFloat(T(TKEY("fog_height_2"), "Fog Height 2"), &settings.fogHeight2, -22000.0f, 22000.0f, "%.1f");
		ImGui::SliderFloat(T(TKEY("fog_height_falloff_2"), "Fog Height Falloff 2"), &settings.fogHeightFalloff2, 0.001f, 2.0f, "%.3f");
		Util::SliderFloat(T(TKEY("fog_density_2"), "Fog Density 2"), settings.fogDensity2, 0.0f, 1.0f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("second_fog_layer_tooltip"),
								  "Adds a second stacked exponential height fog layer with its own base height, density and height falloff.\n"
								  "The two line integrals are summed.\n"
								  "Use it for high-altitude haze above the ground layer or a distinct low-lying ground fog."));
		}
		ImGui::TreePop();
	}
	ImGui::SliderFloat(T(TKEY("dir_inscattering_mul"), "Directional Light Inscattering Multiplier"), &settings.directionalInscatteringMultiplier, 0.0f, 10.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("sunlight_attenuation"), "Sunlight Attenuation Amount"), &settings.sunlightAttenuationAmount, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("dir_inscattering_anisotropy"), "Directional Light Inscattering Anisotropy"), &settings.directionalInscatteringAnisotropy, -0.99f, 0.99f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("dir_inscattering_anisotropy_tooltip"),
							  "Controls the asymmetry of inscattering via the Henyey-Greenstein phase function.\n"
							  "Positive values produce forward scattering (glow around sun).\n"
							  "Zero is isotropic. Negative values produce back scattering."));
	}
	ImGui::Checkbox(T(TKEY("disable_vanilla_fog"), "Disable Vanilla Fog"), (bool*)&settings.disableVanillaFog);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("disable_vanilla_fog_tooltip"), "Disables the vanilla fog entirely. Only exponential height fog will be applied."));
	}
	ImGui::Checkbox(T(TKEY("apply_vanilla_fade"), "Apply Vanilla Fade"), (bool*)&settings.respectVanillaFogFade);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("apply_vanilla_fade_tooltip"), "Applies vanilla fade brightness to exponential height fog."));
	}
	Util::Checkbox(T(TKEY("use_sky_ibl"), "Use Sky IBL for Exterior Inscattering"), settings.useSkyIBL);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_sky_ibl_tooltip"), "Adds the sky IBL color to the fog inscattering in exteriors. Requires the Image Based Lighting feature."));
	}
	Util::Checkbox(T(TKEY("use_dynamic_cubemaps"), "Use Dynamic Cubemaps for Interior Inscattering"), settings.useDynamicCubemaps);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_dynamic_cubemaps_tooltip"), "Adds the dynamic environment cubemap color to the fog inscattering in interiors."));
	}
	ImGui::ColorEdit4(T(TKEY("inscattering_cubemap_tint"), "Inscattering Cubemap Tint"), (float*)&settings.inscatteringTint);
	ImGui::SliderFloat(T(TKEY("cubemap_mip_level"), "Cubemap Mip Level"), &settings.cubemapMipLevel, 1.0f, 8.0f, "%.1f");

	ImGui::TextWrapped("%s", T("feature.view_atmosphere.quality_location", "Grid quality and temporal sampling are configured in Atmosphere and Fog."));
	ImGui::SeparatorText(T(TKEY("volumetric_fog"), "Volumetric Fog"));
	Util::Checkbox(T(TKEY("enable_volumetric_fog"), "Enable Volumetric Fog"), settings.volumetricFogEnabled);
	if (settings.volumetricFogEnabled) {
		ImGui::SliderFloat(T(TKEY("volumetric_start_distance"), "Volumetric Start Distance"), &settings.volumetricFogStartDistance, 0.0f, 20000.0f, "%.0f");
		ImGui::SliderFloat(T(TKEY("near_fade_in_distance"), "Near Fade In Distance"), &settings.volumetricFogNearFadeInDistance, 0.0f, 20000.0f, "%.0f");
		if (ImGui::TreeNode(T(TKEY("volumetric_noise"), "Volumetric Noise"))) {
			ImGui::SliderFloat(T(TKEY("volumetric_noise_scale"), "Noise Scale"), &settings.volumetricFogNoiseScale, 0.0f, 0.01f, "%.6f");
			ImGui::SliderFloat(T(TKEY("volumetric_noise_threshold"), "Noise Threshold"), &settings.volumetricFogNoiseThreshold, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat3(T(TKEY("volumetric_noise_velocity"), "Noise Velocity"), &settings.volumetricFogNoiseVelocity.x, -1.0f, 1.0f, "%.3f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("volumetric_noise_tooltip"),
									  "Modulates the volumetric fog density with a 3D value noise field.\n"
									  "Noise Scale: spatial frequency of the fog clumps (0 = disabled).\n"
									  "Noise Threshold: soft cutoff that carves clumps out of the noise.\n"
									  "Noise Velocity: animation drift of the noise field, scaled by time."));
			}
			ImGui::TreePop();
		}
		Util::SliderFloat(T(TKEY("volumetric_extinction_scale"), "Volumetric Extinction Scale"), settings.volumetricFogExtinctionScale, 0.0f, 10.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("volumetric_scattering_distribution"), "Volumetric Scattering Distribution"), &settings.volumetricFogScatteringDistribution, -0.9f, 0.9f, "%.2f");
		ImGui::ColorEdit4(T(TKEY("volumetric_albedo"), "Volumetric Albedo"), (float*)&settings.volumetricFogAlbedo);
		ImGui::ColorEdit4(T(TKEY("volumetric_emissive"), "Volumetric Emissive"), (float*)&settings.volumetricFogEmissive);
		ImGui::SliderFloat(T(TKEY("directional_scattering_intensity"), "Directional Scattering Intensity"), &settings.volumetricDirectionalScatteringIntensity, 0.0f, 10.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("sky_lighting_scattering_intensity"), "Sky Lighting Scattering Intensity"), &settings.volumetricSkyLightingIntensity, 0.0f, 10.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("local_light_scattering_intensity"), "Local Light Scattering Intensity"), &settings.volumetricLocalLightScatteringIntensity, 0.0f, 10.0f, "%.2f");
	}
}

D3D11_SAMPLER_DESC ExponentialHeightFog::SamplerDesc(bool a_comparison)
{
	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = a_comparison ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	if (a_comparison)
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
	return samplerDesc;
}

void ExponentialHeightFog::PostPostLoad()
{
	Snapshot::RegisterCapture(&ExponentialHeightFog::Capture);
}

void ExponentialHeightFog::Capture(Snapshot::Builder& a_builder)
{
	if (!globals::features::exponentialHeightFog.loaded)
		return;
	const auto* sky = CS_ENGINE_READ(Sky, CapturePoint, globals::game::sky);
	a_builder.Set(Block{ .hideSky = sky && sky->flags.any(RE::Sky::Flags::kHideSky) });
}

void ExponentialHeightFog::SetupResources()
{
	ORGExponentialHeightFog::Get().Setup();
}

void ExponentialHeightFog::EnsureD3D11Objects()
{
	if (volumetricFogCB)
		return;
	auto samplerDesc = SamplerDesc(false);
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	Util::SetResourceName(linearSampler.get(), "ExponentialHeightFog::LinearSampler");

	samplerDesc = SamplerDesc(true);
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, shadowSampler.put()));
	Util::SetResourceName(shadowSampler.get(), "ExponentialHeightFog::ShadowSampler");

	volumetricFogCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<VolumetricFogCB>(), "ExponentialHeightFog::VolumetricFogCB");
}

void ExponentialHeightFog::ClearShaderCache()
{
	if (materialSetupCS) {
		materialSetupCS->Release();
		materialSetupCS = nullptr;
	}
	if (farMaterialSetupCS) {
		farMaterialSetupCS->Release();
		farMaterialSetupCS = nullptr;
	}
	if (conservativeDepthCS) {
		conservativeDepthCS->Release();
		conservativeDepthCS = nullptr;
	}
	if (farConservativeDepthCS) {
		farConservativeDepthCS->Release();
		farConservativeDepthCS = nullptr;
	}
	if (conservativeDepthGroupCS) {
		conservativeDepthGroupCS->Release();
		conservativeDepthGroupCS = nullptr;
	}
	if (farConservativeDepthGroupCS) {
		farConservativeDepthGroupCS->Release();
		farConservativeDepthGroupCS = nullptr;
	}
	if (lightScatteringCS) {
		lightScatteringCS->Release();
		lightScatteringCS = nullptr;
	}
	if (farLightScatteringCS) {
		farLightScatteringCS->Release();
		farLightScatteringCS = nullptr;
	}
	if (integrationCS) {
		integrationCS->Release();
		integrationCS = nullptr;
	}
	if (farIntegrationCS) {
		farIntegrationCS->Release();
		farIntegrationCS = nullptr;
	}
}

ExponentialHeightFog::GridSizes ExponentialHeightFog::ComputeGridSizes() const
{
	const auto& quality = globals::features::viewAtmosphere.settings;
	const auto* graphicsState = CS_ENGINE_READ(GraphicsState, RecordTime, globals::game::graphicsState);
	float2 screenSz{ (float)graphicsState->screenWidth, (float)graphicsState->screenHeight };
	return GridSizesFor(Util::ConvertToDynamic(screenSz), quality.nearPixelSize, quality.nearSlices, quality.middlePixelSize, quality.middleSlices);
}

bool ExponentialHeightFog::VolumesOn(uint32_t a_enabled, uint32_t a_volumetricFogEnabled, float a_extinctionScale, float a_fogDensity, float a_fogDensity2)
{
	return a_enabled && a_volumetricFogEnabled && a_extinctionScale > 0.0f && (a_fogDensity > 0.0f || a_fogDensity2 > 0.0f);
}

ExponentialHeightFog::GridSizes ExponentialHeightFog::GridSizesFor(float2 a_size, uint32_t a_nearPixelSize, uint32_t a_nearSlices, uint32_t a_middlePixelSize,
	uint32_t a_middleSlices)
{
	uint32_t pixelSize = std::clamp(a_nearPixelSize, 4u, 64u);
	const uint32_t gridZ = std::clamp(a_nearSlices, 16u, 160u);
	uint32_t farPixelSize = std::clamp(a_middlePixelSize, 4u, 64u);
	const uint32_t farGridZ = std::clamp(a_middleSlices, 16u, 160u);
	const float2 renderSize = a_size;

	auto getGridSize = [&renderSize](uint32_t a_pixelSize, uint32_t a_gridZ) {
		return DirectX::XMUINT4{
			std::max(1u, static_cast<uint32_t>(std::ceil(renderSize.x / static_cast<float>(a_pixelSize)))),
			std::max(1u, static_cast<uint32_t>(std::ceil(renderSize.y / static_cast<float>(a_pixelSize)))),
			a_gridZ,
			0u
		};
	};
	DirectX::XMUINT4 gridSize = getGridSize(pixelSize, gridZ);

	constexpr uint64_t maxVolumeVoxels = 16ull * 1024ull * 1024ull;
	while (pixelSize < 64u &&
		   static_cast<uint64_t>(gridSize.x) * gridSize.y * gridSize.z > maxVolumeVoxels) {
		pixelSize++;
		gridSize = getGridSize(pixelSize, gridZ);
	}

	// The far volume must be coarser than the near volume.
	farPixelSize = std::max(farPixelSize, pixelSize);
	DirectX::XMUINT4 farGridSize = getGridSize(farPixelSize, farGridZ);
	while (farPixelSize < 64u &&
		   static_cast<uint64_t>(farGridSize.x) * farGridSize.y * farGridSize.z > maxVolumeVoxels / 4ull) {
		farPixelSize++;
		farGridSize = getGridSize(farPixelSize, farGridZ);
	}

	GridSizes sizes;
	sizes.nearSize = gridSize;
	sizes.farSize = farGridSize;
	// From 8-pixel cells up, one thread group per cell reduces the cell's depth footprint in parallel.
	sizes.nearGroupReduce = std::clamp(a_nearPixelSize, 4u, 64u) >= 8;
	sizes.farGroupReduce = sizes.nearGroupReduce || std::clamp(a_middlePixelSize, 4u, 64u) >= 8;
	return sizes;
}

void ExponentialHeightFog::EnsureVolumetricResources()
{
	const auto sizes = ComputeGridSizes();
	const DirectX::XMUINT4 gridSize = sizes.nearSize;
	const DirectX::XMUINT4 farGridSize = sizes.farSize;

	if (vBufferA &&
		currentGridSize.x == gridSize.x && currentGridSize.y == gridSize.y && currentGridSize.z == gridSize.z &&
		currentFarGridSize.x == farGridSize.x && currentFarGridSize.y == farGridSize.y && currentFarGridSize.z == farGridSize.z)
		return;

	currentGridSize = gridSize;
	currentFarGridSize = farGridSize;

	auto make3D = [this](const DirectX::XMUINT4& a_size, const char* a_name, DXGI_FORMAT a_format = DXGI_FORMAT_R16G16B16A16_FLOAT) {
		D3D11_TEXTURE3D_DESC texDesc{};
		texDesc.Width = a_size.x;
		texDesc.Height = a_size.y;
		texDesc.Depth = a_size.z;
		texDesc.MipLevels = 1;
		texDesc.Format = a_format;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
		uavDesc.Texture3D.MipSlice = 0;
		uavDesc.Texture3D.FirstWSlice = 0;
		uavDesc.Texture3D.WSize = a_size.z;

		auto tex = std::make_unique<Texture3D>(texDesc, a_name);
		tex->CreateSRV(srvDesc);
		tex->CreateUAV(uavDesc);
		return tex;
	};

	auto make2D = [this](const DirectX::XMUINT4& a_size, const char* a_name) {
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = a_size.x;
		texDesc.Height = a_size.y;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

		auto tex = std::make_unique<Texture2D>(texDesc, a_name);
		tex->CreateSRV(srvDesc);
		tex->CreateUAV(uavDesc);
		return tex;
	};

	vBufferA = make3D(gridSize, "ExponentialHeightFog::VBufferA");
	conservativeDepth = make2D(gridSize, "ExponentialHeightFog::ConservativeDepth");
	lightScattering = make3D(gridSize, "ExponentialHeightFog::LightScattering", DXGI_FORMAT_R32G32B32A32_FLOAT);
	integratedLightScattering = make3D(gridSize, ORGExponentialHeightFog::kNearIntegratedId.data());

	conservativeDepthHistory = std::make_unique<Texture2D>(conservativeDepth->desc, "ExponentialHeightFog::ConservativeDepthHistory");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = conservativeDepth->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		conservativeDepthHistory->CreateSRV(srvDesc);
	}

	lightScatteringHistory = std::make_unique<Texture3D>(lightScattering->desc, "ExponentialHeightFog::LightScatteringHistory");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = lightScattering->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MipLevels = 1;
		lightScatteringHistory->CreateSRV(srvDesc);
	}

	vBufferAFar = make3D(farGridSize, "ExponentialHeightFog::VBufferAFar");
	conservativeDepthFar = make2D(farGridSize, "ExponentialHeightFog::ConservativeDepthFar");
	lightScatteringFar = make3D(farGridSize, "ExponentialHeightFog::LightScatteringFar", DXGI_FORMAT_R32G32B32A32_FLOAT);
	integratedLightScatteringFar = make3D(farGridSize, ORGExponentialHeightFog::kFarIntegratedId.data());

	conservativeDepthFarHistory = std::make_unique<Texture2D>(conservativeDepthFar->desc, "ExponentialHeightFog::ConservativeDepthFarHistory");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = conservativeDepthFar->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		conservativeDepthFarHistory->CreateSRV(srvDesc);
	}

	lightScatteringFarHistory = std::make_unique<Texture3D>(lightScatteringFar->desc, "ExponentialHeightFog::LightScatteringFarHistory");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = lightScatteringFar->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MipLevels = 1;
		lightScatteringFarHistory->CreateSRV(srvDesc);
	}

	hasLightScatteringHistory = false;
	hasConservativeDepthHistory = false;
	hasLightScatteringFarHistory = false;
	hasConservativeDepthFarHistory = false;
	lastPrepassFrame = UINT32_MAX;
}

void ExponentialHeightFog::ReleaseVolumetricResources()
{
	vBufferA.reset();
	vBufferAFar.reset();
	conservativeDepth.reset();
	conservativeDepthHistory.reset();
	conservativeDepthFar.reset();
	conservativeDepthFarHistory.reset();
	lightScattering.reset();
	lightScatteringHistory.reset();
	lightScatteringFar.reset();
	lightScatteringFarHistory.reset();
	integratedLightScattering.reset();
	integratedLightScatteringFar.reset();
	currentGridSize = {};
	currentFarGridSize = {};
	hasLightScatteringHistory = false;
	hasConservativeDepthHistory = false;
	hasLightScatteringFarHistory = false;
	hasConservativeDepthFarHistory = false;
	lastPrepassFrame = UINT32_MAX;
	ID3D11ShaderResourceView* nullSRV = nullptr;
	globals::d3d::context->PSSetShaderResources(19, 1, &nullSRV);
	globals::d3d::context->PSSetShaderResources(22, 1, &nullSRV);
}

ID3D11ShaderResourceView* ExponentialHeightFog::IntegratedLightScatteringView() const
{
	if (ORGExponentialHeightFog::Debug() == 2)
		return nullptr;
	if (auto* view = ORGExponentialHeightFog::Get().NearSRV())
		return view;
	return integratedLightScattering ? integratedLightScattering->srv.get() : nullptr;
}

ID3D11ShaderResourceView* ExponentialHeightFog::IntegratedLightScatteringFarView() const
{
	if (ORGExponentialHeightFog::Debug() == 2)
		return nullptr;
	if (auto* view = ORGExponentialHeightFog::Get().FarSRV())
		return view;
	return integratedLightScatteringFar ? integratedLightScatteringFar->srv.get() : nullptr;
}

void ExponentialHeightFog::BindIntegratedLightScattering()
{
	// CS_ORG_FOG_DEBUG=5: only the draws that still run on D3D11 get no volume. DCLF reads the views through its own callbacks.
	const bool engineNone = ORGExponentialHeightFog::Debug() == 5;
	ID3D11ShaderResourceView* srv = engineNone ? nullptr : IntegratedLightScatteringView();
	globals::d3d::context->PSSetShaderResources(19, 1, &srv);
	ID3D11ShaderResourceView* farSrv = engineNone ? nullptr : IntegratedLightScatteringFarView();
	globals::d3d::context->PSSetShaderResources(22, 1, &farSrv);
}

ID3D11ShaderResourceView* ExponentialHeightFog::BuildEngineLocalShadowData(EngineLocalShadows& a_shadows) const
{
	auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
	if (!renderer)
		return nullptr;
	auto& depthStencil = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS];
	if (!depthStencil.texture || !depthStencil.depthSRV)
		return nullptr;
	// Native's render side: the casters gave every light's maps back on the game side (the light set's capture), as the engine's masks
	// give them back before this frame's Prepass in guest. No light holds a slice, so no entry is written, and no light is read.
	if (DCLF::Features::Shadows::Native::RenderSet() && !Snapshot::EngineReads::InCapture())
		return depthStencil.depthSRV;
	auto* smState = CS_ENGINE_READ(ShaderManagerState, RecordTime, globals::game::smState);
	if (!smState || !smState->shadowSceneNode[0])
		return nullptr;
	auto* shadowSceneNode = smState->shadowSceneNode[0];

	D3D11_TEXTURE2D_DESC textureDesc{};
	depthStencil.texture->GetDesc(&textureDesc);
	D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
	depthStencil.depthSRV->GetDesc(&viewDesc);
	if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY || textureDesc.Width != textureDesc.Height)
		return nullptr;
	const uint32_t firstSlice = viewDesc.Texture2DArray.FirstArraySlice;
	const uint32_t sliceCount = std::min(textureDesc.ArraySize, LightLimitFix::ENGINE_SHADOW_MAP_SLICES);
	const float texel = 1.0f / static_cast<float>(std::max(textureDesc.Width >> viewDesc.Texture2DArray.MostDetailedMip, 1u));
	std::array<RE::BSShadowLight*, LightLimitFix::SHADOW_MASK_CHANNEL_COUNT> shadowLights{};
	std::array<uint32_t, LightLimitFix::SHADOW_MASK_CHANNEL_COUNT> maskClaims{};
	std::array<uint32_t, LightLimitFix::ENGINE_SHADOW_MAP_SLICES> sliceClaims{};
	const auto& runtimeData = shadowSceneNode->GetRuntimeData();
	for (const auto& entry : runtimeData.activeShadowLights) {
		auto* light = entry.get();
		if (!light || light == runtimeData.sunShadowDirLight || !light->light || !LightLimitFix::IsShadowMapRendered(shadowSceneNode, light))
			continue;
		const auto& lightData = light->GetRuntimeData();
		if (lightData.shadowmapDescriptors.empty())
			continue;
		const auto& descriptor = lightData.shadowmapDescriptors[0];
		const uint32_t slice = descriptor.shadowmapIndex;
		if (descriptor.renderTarget != RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS || slice >= sliceCount ||
			slice < firstSlice || slice - firstSlice >= viewDesc.Texture2DArray.ArraySize)
			continue;
		shadowLights[lightData.maskIndex] = light;
		maskClaims[lightData.maskIndex]++;
		sliceClaims[slice]++;
	}

	auto& shadows = a_shadows;
	const auto& eye = Snapshot::FrameCamera::Main().GetCameraPosAdjust();
	for (uint32_t i = 0; i < shadows.size(); i++) {
		auto* light = shadowLights[i];
		if (!light || maskClaims[i] != 1)
			continue;
		const auto& lightData = light->GetRuntimeData();
		const auto& descriptor = lightData.shadowmapDescriptors[0];
		if (sliceClaims[descriptor.shadowmapIndex] != 1)
			continue;
		uint32_t type = LightLimitFix::LOCAL_SHADOW_TYPE_SPOT;
		float falloff = LightLimitFix::LOCAL_SHADOW_DEFAULT_SPOT_FALLOFF;
		if (light->GetIsParabolicLight()) {
			type = light->shadowMapCount == 2 ? LightLimitFix::LOCAL_SHADOW_TYPE_OMNI : LightLimitFix::LOCAL_SHADOW_TYPE_HEMISPHERE;
		} else if (light->GetIsFrustumLight()) {
			const float spotFalloff = static_cast<RE::BSShadowFrustumLight*>(light)->GetShadowFrustumLightRuntimeData().falloff;
			if (std::isfinite(spotFalloff) && spotFalloff > 0.0f)
				falloff = spotFalloff;
		} else {
			continue;
		}
		auto& data = shadows[i];
		static_assert(sizeof(data.ShadowProj) == sizeof(descriptor.lightTransform));
		std::memcpy(&data.ShadowProj, &descriptor.lightTransform, sizeof(data.ShadowProj));
		for (int column = 0; column < 4; column++)
			data.ShadowProj.m[3][column] = static_cast<float>(static_cast<double>(eye.x) * data.ShadowProj.m[0][column] + static_cast<double>(eye.y) * data.ShadowProj.m[1][column] + static_cast<double>(eye.z) * data.ShadowProj.m[2][column] + static_cast<double>(data.ShadowProj.m[3][column]));
		data.Params = { static_cast<float>(type), light->light->GetLightRuntimeData().radius.x, lightData.shadowBiasScale * LightLimitFix::LOCAL_SHADOW_DEPTH_BIAS, 1.0f };
		data.Params2 = { falloff, 0.0f, static_cast<float>(descriptor.shadowmapIndex - firstSlice), texel };
		data.Origin = { eye.x, eye.y, eye.z, 0.0f };
	}
	return depthStencil.depthSRV;
}

ID3D11ComputeShader* ExponentialHeightFog::GetMaterialSetupCS()
{
	if (!materialSetupCS)
		materialSetupCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogMaterialCS.hlsl", {}, "cs_5_0"));
	return materialSetupCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetConservativeDepthCS(bool a_groupReduce)
{
	if (a_groupReduce) {
		if (!conservativeDepthGroupCS)
			conservativeDepthGroupCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", { { "GROUP_REDUCE", "" } }, "cs_5_0"));
		return conservativeDepthGroupCS;
	}
	if (!conservativeDepthCS)
		conservativeDepthCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", {}, "cs_5_0"));
	return conservativeDepthCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetLightScatteringCS()
{
	if (!lightScatteringCS) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::lightLimitFix.loaded) {
			defines.emplace_back("LIGHT_LIMIT_FIX", "");
		}
		if (globals::features::terrainShadows.loaded) {
			defines.emplace_back("TERRAIN_SHADOWS", "");
		}
		if (globals::features::cloudShadows.loaded) {
			defines.emplace_back("CLOUD_SHADOWS", "");
		}
		if (globals::features::physicalSky.loaded) {
			defines.emplace_back("PHYSICAL_SKY", "");
		}
		lightScatteringCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogLightScatteringCS.hlsl", defines, "cs_5_0"));
	}
	return lightScatteringCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarLightScatteringCS()
{
	if (!farLightScatteringCS) {
		std::vector<std::pair<const char*, const char*>> defines;
		defines.emplace_back("VOLUMETRIC_FOG_FAR_GRID", "");
		if (globals::features::lightLimitFix.loaded) {
			defines.emplace_back("LIGHT_LIMIT_FIX", "");
		}
		if (globals::features::terrainShadows.loaded) {
			defines.emplace_back("TERRAIN_SHADOWS", "");
		}
		if (globals::features::cloudShadows.loaded) {
			defines.emplace_back("CLOUD_SHADOWS", "");
		}
		if (globals::features::physicalSky.loaded) {
			defines.emplace_back("PHYSICAL_SKY", "");
		}
		farLightScatteringCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogLightScatteringCS.hlsl", defines, "cs_5_0"));
	}
	return farLightScatteringCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarMaterialSetupCS()
{
	if (!farMaterialSetupCS)
		farMaterialSetupCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogMaterialCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" } }, "cs_5_0"));
	return farMaterialSetupCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarConservativeDepthCS(bool a_groupReduce)
{
	if (a_groupReduce) {
		if (!farConservativeDepthGroupCS)
			farConservativeDepthGroupCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" }, { "GROUP_REDUCE", "" } }, "cs_5_0"));
		return farConservativeDepthGroupCS;
	}
	if (!farConservativeDepthCS)
		farConservativeDepthCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" } }, "cs_5_0"));
	return farConservativeDepthCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetIntegrationCS()
{
	if (!integrationCS)
		integrationCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogIntegrationCS.hlsl", {}, "cs_5_0"));
	return integrationCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarIntegrationCS()
{
	if (!farIntegrationCS)
		farIntegrationCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogIntegrationCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" } }, "cs_5_0"));
	return farIntegrationCS;
}

ExponentialHeightFog::VolumetricFogCB ExponentialHeightFog::BuildConstants(const ConstantInputs& a_inputs, bool& a_farGridEnabled) const
{
	const auto& quality = globals::features::viewAtmosphere.settings;
	const auto& gridSize = a_inputs.sizes.nearSize;
	const auto& farGridSize = a_inputs.sizes.farSize;

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	// Depth ranges. The near volume spans [nearPlane, nearEndDepth] at full resolution;
	// the far volume continues [nearEndDepth, totalFarPlane] at quarter lattice.
	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	const auto cameraData = Util::GetCameraData();
	const double nearPlane = std::max(static_cast<double>(cameraData.y), static_cast<double>(std::max(settings.volumetricFogStartDistance, 0.0f)));
	const double totalFarPlane = std::max(nearPlane + 1.0, static_cast<double>(std::max(quality.localRange, settings.volumetricFogStartDistance + 1.0f)));
	const double nearEndDepth = std::min(
		std::max(static_cast<double>(std::max(quality.nearRange, 0.0f)), nearPlane + 1.0),
		totalFarPlane);
	a_farGridEnabled = nearEndDepth + 1.0 < totalFarPlane;

	auto computeGridZParams = [](double a_nearPlane, double a_farPlane, uint32_t a_gridZ, float a_distributionScale) {
		constexpr double kNearOffset = 0.095 * 100.0;
		// The offset must stay inside the slab: a fog start at or past Near Grid End leaves a 1-unit near slab.
		const double nearWithOffset = a_nearPlane + std::min(kNearOffset, 0.5 * (a_farPlane - a_nearPlane));
		const double depthDistributionScale = std::max(static_cast<double>(a_distributionScale), static_cast<double>(a_gridZ) / 120.0);
		const double farExp = std::exp2(std::min(static_cast<double>(a_gridZ) / depthDistributionScale, 120.0));
		const double gridZOffset = (a_farPlane - nearWithOffset * farExp) / (a_farPlane - nearWithOffset);
		const double gridZScale = (1.0 - gridZOffset) / nearWithOffset;
		return DirectX::XMFLOAT4{
			static_cast<float>(gridZScale),
			static_cast<float>(gridZOffset),
			static_cast<float>(depthDistributionScale),
			0.0f
		};
	};

	const uint32_t sharedFlags =
		(a_inputs.directionalShadow ? 1u : 0u) |
		(a_inputs.depth ? 2u : 0u) |
		(a_inputs.ibl ? 4u : 0u) |
		(a_inputs.skylighting ? 8u : 0u) |
		(a_inputs.terrainShadows ? 256u : 0u) |
		(a_inputs.cloudShadows ? 512u : 0u) |
		(a_inputs.physSkyTransmittance ? 1024u : 0u) |
		(a_inputs.physSkyShadowVolume ? 2048u : 0u);

	VolumetricFogCB cb{};
	cb.gridSizeAndFlags = {
		gridSize.x,
		gridSize.y,
		gridSize.z,
		sharedFlags |
			(a_inputs.depth && a_inputs.historyValid && a_inputs.depthHistory ? 16u : 0u) |
			(a_inputs.localLights ? 32u : 0u) |
			(a_inputs.localShadows ? 64u : 0u) |
			(a_inputs.engineLocalShadows ? 128u : 0u)
	};
	cb.invGridSizeAndNearFade = {
		1.0f / static_cast<float>(gridSize.x),
		1.0f / static_cast<float>(gridSize.y),
		1.0f / static_cast<float>(gridSize.z),
		settings.volumetricFogNearFadeInDistance > 0.0f ? 1.0f / settings.volumetricFogNearFadeInDistance : 100000000.0f
	};
	cb.gridZParams = computeGridZParams(nearPlane, nearEndDepth, gridSize.z, quality.depthDistribution);

	cb.farGridSizeAndFlags = {
		farGridSize.x,
		farGridSize.y,
		farGridSize.z,
		sharedFlags |
			(a_inputs.depth && a_inputs.historyValidFar && a_inputs.depthHistoryFar ? 16u : 0u)
	};
	cb.farInvGridSizeAndNearFade = {
		1.0f / static_cast<float>(farGridSize.x),
		1.0f / static_cast<float>(farGridSize.y),
		1.0f / static_cast<float>(farGridSize.z),
		settings.volumetricFogNearFadeInDistance > 0.0f ? 1.0f / settings.volumetricFogNearFadeInDistance : 100000000.0f
	};
	cb.farGridZParams = computeGridZParams(nearEndDepth, totalFarPlane, farGridSize.z, quality.depthDistribution);
	cb.farRange = { static_cast<float>(nearEndDepth), static_cast<float>(totalFarPlane), 0.0f, 0.0f };

	cb.clipToWorld = Snapshot::FrameCamera::Main().GetCameraViewProjUnjittered().Invert();

	for (uint32_t i = 0; i < std::size(cb.frameJitterOffsets); i++) {
		const uint32_t temporalFrame = (globals::state->frameCount - i) & 1023u;
		cb.frameJitterOffsets[i] = {
			a_inputs.temporal ? Halton(temporalFrame, 2) : 0.5f,
			a_inputs.temporal ? Halton(temporalFrame, 3) : 0.5f,
			a_inputs.temporal ? Halton(temporalFrame, 5) : 0.5f,
			0.0f
		};
	}
	cb.historyParameters = {
		a_inputs.historyValid ? std::clamp(quality.historyWeight, 0.0f, 0.99f) : 0.0f,
		static_cast<float>(std::clamp(quality.historyMissSamples, 1u, 16u)),
		a_inputs.historyValidFar ? std::clamp(quality.historyWeight, 0.0f, 0.99f) : 0.0f,
		0.0f
	};
	cb.jitterParameters = {
		a_inputs.temporal ? std::max(quality.sampleJitter, 0.0f) : 0.0f,
		static_cast<float>(globals::state->frameCount % 8u),
		0.0f,
		0.0f
	};
	return cb;
}

void ExponentialHeightFog::Prepass()
{
	// The view atmosphere's froxels replace these volumes while it is prepared.
	if (globals::features::viewAtmosphere.IsPrepared()) {
		if (vBufferA || vBufferAFar)
			ReleaseVolumetricResources();
		return;
	}
	// The render-graph path: its volumes and passes follow the adopted revision, and run in the Prepass anchor's epoch.
	// In graph mode the fog never runs on D3D11: a false Prepass means no volumetric fog this frame.
	auto& orgFog = ORGExponentialHeightFog::Get();
	if (orgFog.IsActive()) {
		if (vBufferA || vBufferAFar)
			ReleaseVolumetricResources();
		if (!IsSuppressed() && orgFog.Prepass())
			BindIntegratedLightScattering();
		return;
	}

	if (!settings.enabled || !settings.volumetricFogEnabled || settings.volumetricFogExtinctionScale <= 0.0f) {
		ReleaseVolumetricResources();
		return;
	}

	if (IsSuppressed())
		return;

	// Graph mode never comes here.
	if (ORGExponentialHeightFog::On())
		RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::D3D11Path, RenderGraph::Frame::SegmentId::Count,
			"Exponential Height Fog's volumetric fog (cs.exp-height-fog) ran on D3D11");

	EnsureD3D11Objects();
	EnsureVolumetricResources();

	if (settings.fogDensity <= 0.0f && settings.fogDensity2 <= 0.0f) {
		hasLightScatteringHistory = false;
		hasConservativeDepthHistory = false;
		hasLightScatteringFarHistory = false;
		hasConservativeDepthFarHistory = false;
		lastPrepassFrame = UINT32_MAX;
		BindIntegratedLightScattering();
		return;
	}

	ID3D11ShaderResourceView* directionalShadowLightData = globals::deferred && globals::deferred->directionalShadowLights ? globals::deferred->directionalShadowLights->srv.get() : nullptr;
	auto* sunShadowMap = globals::deferred ? globals::deferred->GetDirectionalShadowMap() : nullptr;
	auto& lightLimitFix = globals::features::lightLimitFix;
	const bool hasLocalLightData =
		lightLimitFix.loaded &&
		lightLimitFix.GetLightsSRV() &&
		lightLimitFix.GetLightIndexListSRV() &&
		lightLimitFix.GetLightGridSRV();
	const bool hasLocalShadowData = hasLocalLightData && lightLimitFix.IsLocalShadowCacheActive() && lightLimitFix.GetLocalShadowDataSRV();
	auto [engineLocalShadowMap, localShadowBuffer] = globals::features::viewAtmosphere.PrepareLocalShadows(hasLocalLightData);
	// The D3D11 dispatch binds it through DXVK's context; the engine's field can be a tripwire handle.
	engineLocalShadowMap = RenderGraph::Engine::Tripwire::Unwrap(engineLocalShadowMap);
	auto* depthSrv = Util::GetCurrentSceneDepthSRV(true);
	auto& ibl = globals::features::ibl;
	auto& physicalSky = globals::features::physicalSky;
	auto& skylighting = globals::features::skylighting;
	const bool hasIBL = ibl.loaded &&
	                    ibl.settings.EnableIBL != 0 &&
	                    !ibl.IsDisabledForCurrentScene() &&
	                    ibl.EnvIBLSRV() &&
	                    ibl.SkyIBLSRV();
	const bool hasPhysicalSky = physicalSky.loaded && physicalSky.texTrLut;
	const bool hasSkylighting = skylighting.loaded && skylighting.ProbeArraySRV();

	const bool temporalReprojection = Util::GetTemporal();
	const bool temporalHistoryValid =
		temporalReprojection &&
		hasLightScatteringHistory &&
		lastPrepassFrame != UINT32_MAX &&
		globals::state->frameCount == lastPrepassFrame + 1u;
	const bool temporalHistoryValidFar =
		temporalReprojection &&
		hasLightScatteringFarHistory &&
		lastPrepassFrame != UINT32_MAX &&
		globals::state->frameCount == lastPrepassFrame + 1u;

	ConstantInputs inputs;
	inputs.sizes = { currentGridSize, currentFarGridSize };
	inputs.directionalShadow = sunShadowMap && directionalShadowLightData;
	inputs.depth = depthSrv != nullptr;
	inputs.ibl = hasIBL;
	inputs.skylighting = hasSkylighting;
	inputs.localLights = hasLocalLightData;
	inputs.localShadows = hasLocalShadowData;
	inputs.engineLocalShadows = engineLocalShadowMap != nullptr;
	// The slots hold whatever the features bound, as before these flags.
	inputs.terrainShadows = true;
	inputs.cloudShadows = true;
	inputs.physSkyTransmittance = true;
	inputs.physSkyShadowVolume = true;
	inputs.temporal = temporalReprojection;
	inputs.historyValid = temporalHistoryValid;
	inputs.historyValidFar = temporalHistoryValidFar;
	inputs.depthHistory = hasConservativeDepthHistory;
	inputs.depthHistoryFar = hasConservativeDepthFarHistory;
	bool farGridEnabled = false;
	const VolumetricFogCB cb = BuildConstants(inputs, farGridEnabled);
	volumetricFogCB->Update(cb);

	auto context = globals::d3d::context;
	ID3D11Buffer* cbuffers[1]{ volumetricFogCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbuffers);
	context->CSSetConstantBuffers(1, 1, &localShadowBuffer);

	ID3D11Buffer* sharedBuffers[2]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, sharedBuffers);

	ID3D11Buffer* frameBuffers[1]{ *CS_ENGINE_READ(BoundState, BoundState, globals::game::perFrame).get() };
	context->CSSetConstantBuffers(12, 1, frameBuffers);

	ID3D11SamplerState* samplers[2]{ linearSampler.get(), shadowSampler.get() };
	context->CSSetSamplers(0, 2, samplers);

	context->CSSetShaderResources(17, 1, &depthSrv);
	ID3D11ShaderResourceView* skylightingSrv = hasSkylighting ? skylighting.ProbeArraySRV() : nullptr;
	ID3D11ShaderResourceView* iblSrvs[2]{
		hasIBL ? ibl.EnvIBLSRV() : nullptr,
		hasIBL ? ibl.SkyIBLSRV() : nullptr
	};
	context->CSSetShaderResources(50, 1, &skylightingSrv);
	ID3D11ShaderResourceView* physicalSkySrvs[4]{
		hasPhysicalSky ? physicalSky.texTrLut->srv.get() : nullptr,
		physicalSky.loaded && physicalSky.texSvLut ? physicalSky.texSvLut->srv.get() : nullptr,
		physicalSky.loaded && physicalSky.texApLut ? physicalSky.texApLut->srv.get() : nullptr,
		physicalSky.loaded && physicalSky.texApShadow ? physicalSky.texApShadow->srv.get() : nullptr
	};
	ID3D11ShaderResourceView* physicalSkyShadowVolumeSrv = physicalSky.loaded && physicalSky.texShadowVolume ? physicalSky.texShadowVolume->srv.get() : nullptr;
	context->CSSetShaderResources(61, 4, physicalSkySrvs);
	context->CSSetShaderResources(76, 2, iblSrvs);
	context->CSSetShaderResources(112, 1, &physicalSkyShadowVolumeSrv);
	ID3D11ShaderResourceView* physicalSkyShadowMeanSrv = physicalSky.loaded && physicalSky.texCloudShadowMean ? physicalSky.texCloudShadowMean->srv.get() : nullptr;
	context->CSSetShaderResources(69, 1, &physicalSkyShadowMeanSrv);
	ID3D11ShaderResourceView* localShadowSrvs[2]{
		hasLocalShadowData ? lightLimitFix.GetLocalShadowDataSRV() : nullptr,
		hasLocalShadowData ? lightLimitFix.GetLocalShadowCacheSRV() : nullptr
	};
	context->CSSetShaderResources(5, 1, &engineLocalShadowMap);
	context->CSSetShaderResources(102, 2, localShadowSrvs);

	struct VolumetricPassDesc
	{
		DirectX::XMUINT4 gridSize;
		Texture3D* vBuffer;
		Texture2D* conservativeDepth;
		Texture2D* conservativeDepthHistory;  // may be null
		Texture3D* scattering;
		Texture3D* scatteringHistory;  // may be null
		Texture3D* integrated;
		ID3D11ComputeShader* materialSetupCS;
		ID3D11ComputeShader* conservativeDepthCS;
		bool groupReduce;
		ID3D11ComputeShader* lightScatteringCS;
		ID3D11ComputeShader* integrationCS;
		bool hasPrevConservativeDepth;
		const char* name;
	};

	auto runVolumetricPass = [&](const VolumetricPassDesc& p) {
		const uint32_t groupX = (p.gridSize.x + 7) / 8;
		const uint32_t groupY = (p.gridSize.y + 7) / 8;
		const uint32_t groupZ = (p.gridSize.z + 3) / 4;

		if (depthSrv) {
			ID3D11UnorderedAccessView* uavs[1]{ p.conservativeDepth->uav.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(p.conservativeDepthCS, nullptr, 0);
			globals::profiler->BeginPass(p.name);
			if (p.groupReduce)
				context->Dispatch(p.gridSize.x, p.gridSize.y, 1);
			else
				context->Dispatch(groupX, groupY, 1);
			globals::profiler->EndPass();
			uavs[0] = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		}

		{
			ID3D11UnorderedAccessView* uavs[1]{ p.vBuffer->uav.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(p.materialSetupCS, nullptr, 0);
			globals::profiler->BeginPass(p.name);
			context->Dispatch(groupX, groupY, groupZ);
			globals::profiler->EndPass();
			uavs[0] = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		}

		{
			ID3D11ShaderResourceView* srvs[5]{
				p.vBuffer->srv.get(),
				sunShadowMap,
				p.scatteringHistory ? p.scatteringHistory->srv.get() : nullptr,
				p.conservativeDepth->srv.get(),
				p.hasPrevConservativeDepth && p.conservativeDepthHistory ? p.conservativeDepthHistory->srv.get() : nullptr
			};
			ID3D11ShaderResourceView* localLightSrvs[3]{
				hasLocalLightData ? lightLimitFix.GetLightsSRV() : nullptr,
				hasLocalLightData ? lightLimitFix.GetLightIndexListSRV() : nullptr,
				hasLocalLightData ? lightLimitFix.GetLightGridSRV() : nullptr
			};
			ID3D11UnorderedAccessView* uavs[1]{ p.scattering->uav.get() };
			context->CSSetShaderResources(0, 5, srvs);
			context->CSSetShaderResources(35, 3, localLightSrvs);
			context->CSSetShaderResources(98, 1, &directionalShadowLightData);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(p.lightScatteringCS, nullptr, 0);
			globals::profiler->BeginPass(p.name);
			context->Dispatch(groupX, groupY, groupZ);
			globals::profiler->EndPass();
			uavs[0] = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		}

		{
			ID3D11ShaderResourceView* srvs[1]{ p.scattering->srv.get() };
			ID3D11UnorderedAccessView* uavs[1]{ p.integrated->uav.get() };
			context->CSSetShaderResources(0, 1, srvs);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(p.integrationCS, nullptr, 0);
			globals::profiler->BeginPass(p.name);
			context->Dispatch(groupX, groupY, 1);
			globals::profiler->EndPass();
		}
	};

	const auto groupReduce = ComputeGridSizes();
	const bool nearGroupReduce = groupReduce.nearGroupReduce;
	const bool farGroupReduce = groupReduce.farGroupReduce;

	runVolumetricPass({ currentGridSize,
		vBufferA.get(),
		conservativeDepth.get(),
		temporalHistoryValid && hasConservativeDepthHistory ? conservativeDepthHistory.get() : nullptr,
		lightScattering.get(),
		temporalHistoryValid ? lightScatteringHistory.get() : nullptr,
		integratedLightScattering.get(),
		GetMaterialSetupCS(),
		GetConservativeDepthCS(nearGroupReduce),
		nearGroupReduce,
		GetLightScatteringCS(),
		GetIntegrationCS(),
		temporalHistoryValid && hasConservativeDepthHistory,
		"ExponentialHeightFog::NearVolume" });

	if (farGridEnabled) {
		runVolumetricPass({ currentFarGridSize,
			vBufferAFar.get(),
			conservativeDepthFar.get(),
			temporalHistoryValidFar && hasConservativeDepthFarHistory ? conservativeDepthFarHistory.get() : nullptr,
			lightScatteringFar.get(),
			temporalHistoryValidFar ? lightScatteringFarHistory.get() : nullptr,
			integratedLightScatteringFar.get(),
			GetFarMaterialSetupCS(),
			GetFarConservativeDepthCS(farGroupReduce),
			farGroupReduce,
			GetFarLightScatteringCS(),
			GetFarIntegrationCS(),
			temporalHistoryValidFar && hasConservativeDepthFarHistory,
			"ExponentialHeightFog::FarVolume" });
	} else {
		const float clearValue[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
		context->ClearUnorderedAccessViewFloat(integratedLightScatteringFar->uav.get(), clearValue);
		hasLightScatteringFarHistory = false;
		hasConservativeDepthFarHistory = false;
	}

	ID3D11ShaderResourceView* nullSrvs[5]{ nullptr, nullptr, nullptr, nullptr, nullptr };
	ID3D11ShaderResourceView* nullDepthSrv[1]{ nullptr };
	ID3D11UnorderedAccessView* nullUav[1]{ nullptr };
	ID3D11SamplerState* nullSamplers[2]{ nullptr, nullptr };
	ID3D11Buffer* nullCb[1]{ nullptr };
	context->CSSetShaderResources(0, 5, nullSrvs);
	context->CSSetShaderResources(5, 1, nullSrvs);
	context->CSSetShaderResources(17, 1, nullDepthSrv);
	context->CSSetShaderResources(35, 3, nullSrvs);
	context->CSSetShaderResources(50, 1, nullDepthSrv);
	context->CSSetShaderResources(76, 2, nullSrvs);
	context->CSSetShaderResources(98, 1, nullSrvs);
	context->CSSetShaderResources(102, 2, nullSrvs);
	context->CSSetShaderResources(112, 1, nullSrvs);
	context->CSSetShaderResources(69, 1, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
	context->CSSetSamplers(0, 2, nullSamplers);
	context->CSSetConstantBuffers(0, 1, nullCb);
	context->CSSetConstantBuffers(1, 1, nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	if (temporalReprojection) {
		context->CopyResource(lightScatteringHistory->resource.get(), lightScattering->resource.get());
		hasLightScatteringHistory = true;
		if (depthSrv) {
			context->CopyResource(conservativeDepthHistory->resource.get(), conservativeDepth->resource.get());
			hasConservativeDepthHistory = true;
		} else {
			hasConservativeDepthHistory = false;
		}
		if (farGridEnabled) {
			context->CopyResource(lightScatteringFarHistory->resource.get(), lightScatteringFar->resource.get());
			hasLightScatteringFarHistory = true;
			if (depthSrv) {
				context->CopyResource(conservativeDepthFarHistory->resource.get(), conservativeDepthFar->resource.get());
				hasConservativeDepthFarHistory = true;
			} else {
				hasConservativeDepthFarHistory = false;
			}
		} else {
			hasLightScatteringFarHistory = false;
			hasConservativeDepthFarHistory = false;
		}
	} else {
		hasLightScatteringHistory = false;
		hasConservativeDepthHistory = false;
		hasLightScatteringFarHistory = false;
		hasConservativeDepthFarHistory = false;
	}

	lastPrepassFrame = globals::state->frameCount;
	BindIntegratedLightScattering();
}
#undef I18N_KEY_PREFIX

#pragma once

#include "Buffer.h"
#include "Features/LightLimitFix.h"
#include "Utils/Setting.h"

struct ExponentialHeightFog : Feature
{
private:
	static constexpr std::string_view MOD_ID = "180146";

public:
	/** @brief Binds the shape settings to their frame variables. The global is the only instance. */
	ExponentialHeightFog();

	virtual inline std::string GetName() override { return "Exponential Height Fog"; }
	virtual std::string GetDisplayName() override { return T("feature.exponential_height_fog.name", "Exponential Height Fog"); }
	virtual inline std::string GetShortName() override { return "ExponentialHeightFog"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.exponential_height_fog.description", "Exponential Height Fog adds a realistic fog effect that increases in density with height, enhancing atmospheric depth and immersion in the game environment."),
			{ T("feature.exponential_height_fog.key_feature_1", "Added exponential height fog effect"),
				T("feature.exponential_height_fog.key_feature_2", "Adapted to vanilla fog settings"),
				T("feature.exponential_height_fog.key_feature_3", "Creates atmospheric depth") } };
	};

	virtual inline std::string_view GetShaderDefineName() override { return "EXP_HEIGHT_FOG"; }
	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	/** @brief Draws the ImGui settings UI for fog parameters and volumetric fog options. */
	virtual void DrawSettings() override;
	/** @brief Sets up the render-graph path when requested; the D3D11 path makes its objects when it first runs. */
	virtual void SetupResources() override;
	/** @brief Releases all cached volumetric fog compute shaders so they can be recompiled. */
	virtual void ClearShaderCache() override;
	/**
	 * @brief Runs the volumetric fog pipeline: material setup, conservative depth,
	 * light scattering, and front-to-back integration.
	 */
	virtual void Prepass() override;

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/** @brief The settings block the shaders read (the FeatureData copy). Its layout is fixed. */
	struct alignas(16) GpuSettings
	{
		uint enabled = 0;
		uint useDynamicCubemaps = 1;
		float startDistance = 0.0f;
		float fogHeight = 0.0f;
		float fogHeightFalloff = 0.2f;
		float fogDensity = 0.005f;
		float fogHeight2 = 0.0f;
		float fogHeightFalloff2 = 0.2f;
		float fogDensity2 = 0.0f;
		float directionalInscatteringMultiplier = 1.0f;
		float directionalInscatteringAnisotropy = 0.2f;
		uint useSkyIBL = 1;
		float4 inscatteringTint = { 1.0f, 1.0f, 1.0f, 1.0f };
		float cubemapMipLevel = 8.0f;
		float sunlightAttenuationAmount = 1.0f;
		uint respectVanillaFogFade = 0;
		uint disableVanillaFog = 1;
		float4 fogInscatteringColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		float originalFogColorAmount = 0.0f;
		uint volumetricFogEnabled = 0;
		float volumetricFogStartDistance = 0.0f;
		float volumetricFogNearFadeInDistance = 1000.0f;
		float4 volumetricFogAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
		float4 volumetricFogEmissive = { 0.0f, 0.0f, 0.0f, 0.0f };
		float volumetricDirectionalScatteringIntensity = 1.0f;
		float volumetricSkyLightingIntensity = 1.0f;
		float volumetricFogScatteringDistribution = 0.2f;
		float volumetricFogExtinctionScale = 1.0f;
		float volumetricLocalLightScatteringIntensity = 1.0f;
		float volumetricFogNoiseScale = 0.0f;  // noise frequency per unit; 0 = disabled
		float volumetricFogNoiseThreshold = 0.5f;
		float pad3 = 0.0f;
		float3 volumetricFogNoiseVelocity = { 0.0f, 0.0f, 0.0f };
		float pad0 = 0.0f;
		float4 originalFogColorOverride = { 1.0f, 1.0f, 1.0f, 0.0f };  // rgb = replaces the weather fog colour, a = blend amount
	};
	STATIC_ASSERT_ALIGNAS_16(GpuSettings);
	static_assert(sizeof(GpuSettings) == 208);

	/** @brief The CPU settings. The ones that change the frame's shape are CS::Setting. */
	struct Settings
	{
		CS::Setting<std::uint32_t> enabled = 0;
		CS::Setting<std::uint32_t> useDynamicCubemaps = 1;
		float startDistance = 0.0f;
		float fogHeight = 0.0f;
		float fogHeightFalloff = 0.2f;
		CS::Setting<float> fogDensity = 0.005f;
		float fogHeight2 = 0.0f;
		float fogHeightFalloff2 = 0.2f;
		CS::Setting<float> fogDensity2 = 0.0f;
		float directionalInscatteringMultiplier = 1.0f;
		float directionalInscatteringAnisotropy = 0.2f;
		CS::Setting<std::uint32_t> useSkyIBL = 1;
		float4 inscatteringTint = { 1.0f, 1.0f, 1.0f, 1.0f };
		float cubemapMipLevel = 8.0f;
		float sunlightAttenuationAmount = 1.0f;
		uint respectVanillaFogFade = 0;
		uint disableVanillaFog = 1;
		float4 fogInscatteringColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		float originalFogColorAmount = 0.0f;
		CS::Setting<std::uint32_t> volumetricFogEnabled = 0;
		float volumetricFogStartDistance = 0.0f;
		float volumetricFogNearFadeInDistance = 1000.0f;
		float4 volumetricFogAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
		float4 volumetricFogEmissive = { 0.0f, 0.0f, 0.0f, 0.0f };
		float volumetricDirectionalScatteringIntensity = 1.0f;
		float volumetricSkyLightingIntensity = 1.0f;
		float volumetricFogScatteringDistribution = 0.2f;
		CS::Setting<float> volumetricFogExtinctionScale = 1.0f;
		float volumetricLocalLightScatteringIntensity = 1.0f;
		float volumetricFogNoiseScale = 0.0f;  // noise frequency per unit; 0 = disabled
		float volumetricFogNoiseThreshold = 0.5f;
		float3 volumetricFogNoiseVelocity = { 0.0f, 0.0f, 0.0f };
		float4 originalFogColorOverride = { 1.0f, 1.0f, 1.0f, 0.0f };  // rgb = replaces the weather fog colour, a = blend amount
	} settings;

	/** @brief The GPU block built from the settings, with the colours in the working space. */
	GpuSettings GetCommonBufferData() const;
	/** @brief Reports whether height fog contributes in the current scene. */
	bool IsActive() const { return loaded && settings.enabled && !IsSuppressed(); }

	/** @brief The view Prepass binds at t19: the render graph's when it ran the last frame, else D3D11's (null before it exists). Render thread. */
	ID3D11ShaderResourceView* IntegratedLightScatteringView() const;

	/** @brief The view Prepass binds at t22, the far volume's, chosen as IntegratedLightScatteringView chooses. Render thread. */
	ID3D11ShaderResourceView* IntegratedLightScatteringFarView() const;

	// The volumetric fog's frame description, shared by the D3D11 path and the render-graph path (ORGExponentialHeightFog).

	/** @brief The volumetric fog passes' constants (VolumetricFogCB, b0; the shaders' VolumetricFogConstants). */
	struct VolumetricFogCB
	{
		DirectX::XMUINT4 gridSizeAndFlags = {};
		float4 invGridSizeAndNearFade = {};
		float4 gridZParams = {};
		float4x4 clipToWorld = {};
		float4 frameJitterOffsets[16] = {};
		float4 historyParameters = {};  // x = near weight, y = miss sample count, z = far weight, w = 1 turns off the scattering's history check (a CS_ORG_FOG_DEBUG diagnostic)
		float4 jitterParameters = {};   // x = LightScatteringSampleJitterMultiplier, y = StateFrameIndexMod8, zw = unused
		// The far grid continues from the near grid to the shared local range.
		DirectX::XMUINT4 farGridSizeAndFlags = {};
		float4 farInvGridSizeAndNearFade = {};
		float4 farGridZParams = {};
		float4 farRange = {};  // x = far start depth, y = far end depth, zw = unused
	};
	STATIC_ASSERT_ALIGNAS_16(VolumetricFogCB);

	/** @brief The two volumes' grid sizes (x, y, slices) and whether their conservative depth reduces a cell per thread group. */
	struct GridSizes
	{
		DirectX::XMUINT4 nearSize = {};
		DirectX::XMUINT4 farSize = {};
		bool nearGroupReduce = false;
		bool farGroupReduce = false;
	};

	/** @brief What a frame's constants depend on besides the settings: the inputs present and the history state. */
	struct ConstantInputs
	{
		GridSizes sizes;
		bool directionalShadow = false;
		bool depth = false;
		bool ibl = false;
		bool skylighting = false;
		bool localLights = false;
		bool localShadows = false;
		bool engineLocalShadows = false;
		// The world shadow inputs the scattering reads only while bound (flags 256 to 2048). The D3D11 path binds them all.
		bool terrainShadows = false;
		bool cloudShadows = false;
		bool physSkyTransmittance = false;
		bool physSkyShadowVolume = false;
		bool temporal = false;
		bool historyValid = false;
		bool historyValidFar = false;
		bool depthHistory = false;
		bool depthHistoryFar = false;
	};

	/** @brief The engine's local shadow lights, one per shadow mask channel (EngineLocalShadowCB, b1). */
	using EngineLocalShadows = std::array<LightLimitFix::LocalShadowData, LightLimitFix::SHADOW_MASK_CHANNEL_COUNT>;

	/**
	 * @brief Fills a_shadows with the engine's local shadow lights. Atmosphere and Fog's volume reads the same data.
	 * @return The engine's shadow map array view they index, or null when there is none (a_shadows untouched).
	 */
	ID3D11ShaderResourceView* BuildEngineLocalShadowData(EngineLocalShadows& a_shadows) const;

private:
	friend class ORGExponentialHeightFog;

	/** @brief The volumes' grid sizes for the current render size and live settings. The D3D11 path's. */
	GridSizes ComputeGridSizes() const;

	/** @brief The volumes' grid sizes for a_size (the area the grid covers) and the grid settings. */
	static GridSizes GridSizesFor(float2 a_size, uint32_t a_nearPixelSize, uint32_t a_nearSlices, uint32_t a_middlePixelSize, uint32_t a_middleSlices);

	/** @brief Whether the volumetric fog's volumes exist for these settings. */
	static bool VolumesOn(uint32_t a_enabled, uint32_t a_volumetricFogEnabled, float a_extinctionScale, float a_fogDensity, float a_fogDensity2);

	/** @brief The frame's volumetric fog constants; a_farGridEnabled says whether the far volume covers any depth. */
	VolumetricFogCB BuildConstants(const ConstantInputs& a_inputs, bool& a_farGridEnabled) const;

	/** @brief The D3D11 path's samplers (s0 linear clamp, s1 the shadow comparison), which the render-graph path converts. */
	static D3D11_SAMPLER_DESC SamplerDesc(bool a_comparison);

	/** @brief Shaders see height fog as disabled: an ENB preset replaces it without handing it back to CS, or the flat world map is open. */
	bool IsSuppressed() const;
	/** @brief The D3D11 path's samplers and constant buffer, made the first time it runs. */
	void EnsureD3D11Objects();
	void EnsureVolumetricResources();
	void ReleaseVolumetricResources();
	void BindIntegratedLightScattering();
	ID3D11ComputeShader* GetMaterialSetupCS();
	ID3D11ComputeShader* GetFarMaterialSetupCS();
	ID3D11ComputeShader* GetConservativeDepthCS(bool a_groupReduce);
	ID3D11ComputeShader* GetFarConservativeDepthCS(bool a_groupReduce);
	ID3D11ComputeShader* GetLightScatteringCS();
	ID3D11ComputeShader* GetFarLightScatteringCS();
	ID3D11ComputeShader* GetIntegrationCS();
	ID3D11ComputeShader* GetFarIntegrationCS();

	std::unique_ptr<Texture3D> vBufferA;
	std::unique_ptr<Texture3D> vBufferAFar;
	std::unique_ptr<Texture2D> conservativeDepth;
	std::unique_ptr<Texture2D> conservativeDepthHistory;
	std::unique_ptr<Texture2D> conservativeDepthFar;
	std::unique_ptr<Texture2D> conservativeDepthFarHistory;
	std::unique_ptr<Texture3D> lightScattering;
	std::unique_ptr<Texture3D> lightScatteringHistory;
	std::unique_ptr<Texture3D> lightScatteringFar;
	std::unique_ptr<Texture3D> lightScatteringFarHistory;
	std::unique_ptr<Texture3D> integratedLightScattering;
	std::unique_ptr<Texture3D> integratedLightScatteringFar;
	std::unique_ptr<ConstantBuffer> volumetricFogCB;
	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11SamplerState> shadowSampler;
	ID3D11ComputeShader* materialSetupCS = nullptr;
	ID3D11ComputeShader* farMaterialSetupCS = nullptr;
	ID3D11ComputeShader* conservativeDepthCS = nullptr;
	ID3D11ComputeShader* farConservativeDepthCS = nullptr;
	ID3D11ComputeShader* conservativeDepthGroupCS = nullptr;
	ID3D11ComputeShader* farConservativeDepthGroupCS = nullptr;
	ID3D11ComputeShader* lightScatteringCS = nullptr;
	ID3D11ComputeShader* farLightScatteringCS = nullptr;
	ID3D11ComputeShader* integrationCS = nullptr;
	ID3D11ComputeShader* farIntegrationCS = nullptr;
	DirectX::XMUINT4 currentGridSize = {};
	DirectX::XMUINT4 currentFarGridSize = {};
	bool hasLightScatteringHistory = false;
	bool hasConservativeDepthHistory = false;
	bool hasLightScatteringFarHistory = false;
	bool hasConservativeDepthFarHistory = false;
	uint32_t lastPrepassFrame = UINT32_MAX;
};

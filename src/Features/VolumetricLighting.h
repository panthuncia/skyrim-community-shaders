#pragma once

#include "Utils/Setting.h"

/** @brief Adds configurable volumetric lighting with god rays and atmospheric scattering effects. */
struct VolumetricLighting : Feature
{
public:
	/** @brief Dimensions for the volumetric lighting 3D texture. */
	struct TextureSize
	{
		int32_t Width = 320;
		int32_t Height = 192;
		int32_t Depth = 90;
		friend bool operator==(const TextureSize&, const TextureSize&) = default;
	};

	/** @brief The volume's size and the engine's VL on/off follow these, so they are CS::Setting. */
	struct Settings
	{
		CS::Setting<bool> ExteriorEnabled = true;
		CS::Setting<int32_t> ExteriorQuality = 2;
		CS::Setting<TextureSize> ExteriorCustomSize;
		CS::Setting<bool> InteriorEnabled = true;
		CS::Setting<int32_t> InteriorQuality = 2;
		CS::Setting<TextureSize> InteriorCustomSize;
	};

	/** @brief Binds the settings to their frame variables. The global is the only instance. */
	VolumetricLighting();

	Settings settings;

	virtual inline std::string GetName() override { return "Volumetric Lighting"; }
	virtual std::string GetDisplayName() override { return T("feature.volumetric_lighting.name", "Volumetric Lighting"); }
	/** @brief Returns the short identifier used for file paths and logging. */
	virtual inline std::string GetShortName() override { return "VolumetricLighting"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	/** @brief Returns a summary description and list of key features for the UI. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.volumetric_lighting.description", "Volumetric Lighting creates realistic light scattering effects through fog, dust, and atmospheric particles.\nThis adds dramatic god rays and atmospheric depth to both interior and exterior environments."),
			{ T("feature.volumetric_lighting.key_feature_1", "Realistic light scattering"),
				T("feature.volumetric_lighting.key_feature_2", "God rays and atmospheric effects"),
				T("feature.volumetric_lighting.key_feature_3", "Separate interior/exterior settings"),
				T("feature.volumetric_lighting.key_feature_4", "Configurable quality levels"),
				T("feature.volumetric_lighting.key_feature_5", "Enhanced atmospheric immersion") } };
	};

	virtual void SaveSettings(json&) override;
	virtual void LoadSettings(json&) override;
	virtual void RestoreDefaultSettings() override;
	/** @brief Draws the ImGui settings panel for volumetric lighting configuration. */
	virtual void DrawSettings() override;
	/** @brief Handles post-data-load initialization. */
	virtual void DataLoaded() override;
	/** @brief Resolves game engine addresses and patches the raymarch dispatch loop. */
	virtual void PostPostLoad() override;
	/** @brief Creates the volumetric lighting constant buffer. */
	virtual void SetupResources() override;
	/**
	 * @brief Updates screen dimensions, writes the Interior frame variable, and gives the engine its VL quality. On the render
	 * graph the quality is the adopted revision's, so the engine's volume size matches the declared volume.
	 */
	virtual void EarlyPrepass() override;

	virtual bool IsCore() const override { return true; };

	/**
	 * @brief Creates a BSImagespaceShader wrapping a compute shader for volumetric lighting passes.
	 * @param name The shader's internal name.
	 * @param fileName The FXP filename for the shader.
	 * @param computeShader The compute shader to wrap.
	 * @return The created BSImagespaceShader instance.
	 */
	static RE::BSImagespaceShader* CreateShader(const std::string_view& name, const std::string_view& fileName, RE::BSComputeShader* computeShader);
	/**
	 * @brief Returns the density generation compute shader, creating it on first call.
	 * @param computeShader The compute shader to wrap if creation is needed.
	 * @return The cached BSImagespaceShader for the generate pass.
	 */
	RE::BSImagespaceShader* GetOrCreateGenerateCS(RE::BSComputeShader* computeShader);
	/**
	 * @brief Returns the raymarching compute shader, creating it on first call.
	 * @param computeShader The compute shader to wrap if creation is needed.
	 * @return The cached BSImagespaceShader for the raymarch pass.
	 */
	RE::BSImagespaceShader* GetOrCreateRaymarchCS(RE::BSComputeShader* computeShader);
	/**
	 * @brief Returns the horizontal blur compute shader, creating it on first call.
	 * @param computeShader The compute shader to wrap if creation is needed.
	 * @return The cached BSImagespaceShader for the horizontal blur pass.
	 */
	RE::BSImagespaceShader* GetOrCreateBlurHCS(RE::BSComputeShader* computeShader);
	/**
	 * @brief Returns the vertical blur compute shader, creating it on first call.
	 * @param computeShader The compute shader to wrap if creation is needed.
	 * @return The cached BSImagespaceShader for the vertical blur pass.
	 */
	RE::BSImagespaceShader* GetOrCreateBlurVCS(RE::BSComputeShader* computeShader);
	/** @brief Binds the screen dimensions constant buffer to compute shader slot 1. */
	void SetDimensionsCB() const;
	/**
	 * @brief Calculates the thread group counts for the horizontal blur dispatch.
	 *
	 * Both axes are sized from the dynamic resolution render area so the blur never
	 * touches the stale region outside it.
	 *
	 * @param threadGroupCountX Output parameter set to the required X thread group count.
	 * @param threadGroupCountY Output parameter set to the number of rows to blur.
	 */
	void SetGroupCountsHCS(uint32_t& threadGroupCountX, uint32_t& threadGroupCountY) const;
	/**
	 * @brief Calculates the thread group counts for the vertical blur dispatch.
	 *
	 * Both axes are sized from the dynamic resolution render area so the blur never
	 * touches the stale region outside it.
	 *
	 * @param threadGroupCountX Output parameter set to the number of columns to blur.
	 * @param threadGroupCountY Output parameter set to the required Y thread group count.
	 */
	void SetGroupCountsVCS(uint32_t& threadGroupCountX, uint32_t& threadGroupCountY) const;

	/**
	 * @brief The volume size the engine is given for a quality (its preset, or a_custom for Custom), in texels. The ORG path
	 * declares its volume with it.
	 */
	TextureSize VolumeSizeFor(int32_t a_quality, const TextureSize& a_custom) const;

private:
	struct VolumetricLightingDescriptor
	{};

	static const char* FromUnits(int32_t value, int32_t unitScale);
	static VolumetricLightingDescriptor& GetVLDescriptor();
	static void SetVLQuality(VolumetricLightingDescriptor& descriptor, std::uint32_t quality);
	void DrawVolumetricLightingSettings(CS::Setting<int32_t>& quality, CS::Setting<TextureSize>& customSize, bool isInterior);
	TextureSize& FetchCurrentSizeInUnits(bool interior);

	/** @brief What the engine is given: VL on or off, the quality, and the High preset's size (the custom size for Custom). */
	struct EngineVL
	{
		bool enabled = false;
		int32_t quality = 0;
		TextureSize highSize;
		friend bool operator==(const EngineVL&, const EngineVL&) = default;
	};
	/** @brief From the live settings, or on the render graph from the adopted revision's. */
	EngineVL WantedEngineVL() const;
	void ApplyEngineVL(const EngineVL& a_vl);

	enum class Quality : uint8_t
	{
		Low,
		Medium,
		High,
		Custom,
		Count
	};

	const char* QualityNames[static_cast<uint8_t>(Quality::Count)] = { "Low", "Medium", "High", "Custom" };

	TextureSize exteriorSizeInUnits;
	TextureSize interiorSizeInUnits;
	TextureSize defaultSizeHigh;

	TextureSize* gVolumetricLightingSizeHigh = nullptr;
	TextureSize* gVolumetricLightingSizeMedium = nullptr;
	TextureSize* gVolumetricLightingSizeLow = nullptr;

	// The location as the live cell gives it. The engine is given the adopted location's settings on the render graph.
	bool inInterior = false;
	bool inInteriorWithSun = false;
	std::optional<EngineVL> appliedVL;

	/**
	 * @brief Bounds of the dynamic resolution render area, in pixels.
	 *
	 * The blur targets are allocated at the full screen size but only the top-left
	 * dynamic resolution region holds valid data, so these are the clamp bounds the
	 * blur shaders sample against.
	 */
	struct VLData
	{
		int32_t screenX;
		int32_t screenY;
		int32_t screenXMin1;
		int32_t screenYMin1;
	};
	VLData vlData = VLData();

	// Full (non-dynamic) screen size, tracked separately so the cached blur shaders are
	// only invalidated on an actual resolution change and not every dynamic resolution step.
	int32_t fullScreenX = 0;
	int32_t fullScreenY = 0;
	ConstantBuffer* vlDataCB = nullptr;

	static constexpr int32_t BlurThreadGroupSizeX = 256;
	static constexpr int32_t BlurThreadGroupSizeY = 256;
	static constexpr int32_t BlurWindow = 12;

	RE::BSImagespaceShader* generateCS = nullptr;
	RE::BSImagespaceShader* raymarchCS = nullptr;
	RE::BSImagespaceShader* blurHCS = nullptr;
	RE::BSImagespaceShader* blurVCS = nullptr;
};

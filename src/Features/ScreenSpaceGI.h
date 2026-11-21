#pragma once

#include "Buffer.h"
#include "Utils/Setting.h"

struct ScreenSpaceGI : Feature
{
private:
	static constexpr std::string_view MOD_ID = "130375";

public:
	/** @brief Binds the shape settings to their frame variables. The global is the only instance. */
	ScreenSpaceGI();

	virtual inline std::string GetName() override { return "Screen Space GI"; }
	virtual std::string GetDisplayName() override { return T("feature.screen_space_gi.name", "Screen Space GI"); }
	virtual inline std::string GetShortName() override { return "ScreenSpaceGI"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		std::string desc = T("feature.screen_space_gi.description", "Screen Space Global Illumination adds realistic indirect lighting and ambient occlusion to the game. This technique simulates how light bounces off surfaces to illuminate other objects naturally.");
		return { desc,
			{ T("feature.screen_space_gi.key_feature_1", "Realistic indirect lighting"),
				T("feature.screen_space_gi.key_feature_2", "Enhanced ambient occlusion"),
				T("feature.screen_space_gi.key_feature_3", "Improved visual depth and atmosphere"),
				T("feature.screen_space_gi.key_feature_4", "Temporal denoising for smooth results"),
				T("feature.screen_space_gi.key_feature_5", "Configurable quality and performance settings") } };
	}

	/** @brief Resets all settings to their default values. */
	virtual void RestoreDefaultSettings() override;
	/** @brief Draws the ImGui settings UI with quality presets, visual parameters, and denoising options. */
	virtual void DrawSettings() override;
	virtual bool HasDebugSection() const override { return true; }
	virtual void DrawDebugSection() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/** @brief The quality presets of the menu's buttons. */
	enum class Preset
	{
		AOOnly,
		Low,
		Standard,
		Extreme,
		Reference,
	};

	/** @brief Writes a preset's settings, as its menu button does. */
	void ApplyPreset(Preset a_preset);

	/** @brief Whether this frame's composite takes the specular GI texture: the adopted value on the graph path, else the setting. */
	bool FrameSpecularGI() const;

	/** @brief Registers the loading screen listener that resets the temporal history. */
	virtual void PostPostLoad() override;
	/** @brief Creates GPU textures, samplers, constant buffers, and compiles compute shaders. */
	virtual void SetupResources() override;
	/** @brief Creates the D3D11 path's textures, sized as kMAIN (SetupResources, or the first D3D11 frame). */
	void CreateTextures();
	/** @brief Before the DeferredPasses anchor: the graph path notes new engine inputs. Its programs, textures and client follow the frame revisions. */
	virtual void Prepass() override;
	/** @brief Releases and recompiles all SSGI compute shaders. */
	virtual void ClearShaderCache() override;
	/** @brief Compiles all SSGI compute shaders with current resolution and feature defines. */
	void CompileComputeShaders();

	/** @brief The settings the D3D11 shaders' defines come from. DrawSSGI recompiles when they differ from the compiled ones. */
	struct CompiledDefines
	{
		int resolutionMode = -1;
		bool temporal = false;
		bool gi = false;
		bool specular = false;

		bool operator==(const CompiledDefines&) const = default;
	};
	CompiledDefines CurrentDefines() const;
	CompiledDefines compiledDefines;
	/** @brief Checks whether all required compute shaders and the noise texture loaded successfully. */
	bool ShadersOK();

	/** @brief Executes the full SSGI pipeline: depth prefilter, radiance fetch, GI, blur, and upsample. */
	void DrawSSGI();
	/** @brief Updates the SSGI constant buffer with current camera, resolution, and settings data. */
	void UpdateSB();

	/** @brief The dispatch sizes: the dynamic render size, and the size the chain works at (half or quarter of it). */
	struct DispatchSizes
	{
		std::array<uint, 2> full, internal;
	};
	DispatchSizes GetDispatchSizes() const;

	//////////////////////////////////////////////////////////////////////////////////

	uint outputAoIdx = 0;
	uint outputIlIdx = 0;

	// Loading screen radiance decays at only 1/MaxAccumFrames per frame, bleeding the old scene
	// into the new one for some frames.
	std::atomic<bool> queuedResetHistory{ true };
	// Graph mode: each reset the render thread takes from queuedResetHistory. The history textures are declared on it, so a reset
	// is a new version of each, born zero. Not a saved setting.
	CS::Setting<std::uint32_t> historyResets = 0;

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

		static bool Register();
	};

	/** @brief The shape settings are CS::Setting (frame revision plan 6.6): they set the graph path's programs, textures and client. */
	struct Settings
	{
		CS::Setting<bool> Enabled = true;
		CS::Setting<bool> EnableGI = true;
		CS::Setting<bool> EnableExperimentalSpecularGI = false;
		bool EnableVanillaSSAO = false;
		// performance/quality
		uint NumSlices = 4u;
		uint NumSteps = 8u;
		CS::Setting<int> ResolutionMode = 1;  // 0-full, 1-half, 2-quarter - DBF default
		// visual
		float MinScreenRadius = 0.01f;
		float AORadius = 256.f;
		float GIRadius = 256.f;
		float Thickness = 32.f;
		float2 DepthFadeRange = { 4e4, 5e4 };
		// gi
		float GISaturation = 0.8f;
		float GIDistanceCompensation = 0.f;
		// mix
		float AOPower = 1.0f;
		float GIStrength = 1.0f;
		// denoise
		CS::Setting<bool> EnableTemporalDenoiser = true;
		CS::Setting<bool> EnableBlur = true;
		float DepthDisocclusion = .1f;
		float NormalDisocclusion = .1f;
		uint MaxAccumFrames = 16;
		float BlurRadius = 2.f;
		float DistanceNormalisation = 2.f;
	} settings;

	struct alignas(16) SSGICB
	{
		float4x4 PrevInvViewMat;
		float4 NDCToViewMul;
		float4 NDCToViewAdd;

		float2 TexDim;
		float2 RcpTexDim;  //
		float2 FrameDim;
		float2 RcpFrameDim;  //
		float pad0;

		uint NumSlices;
		uint NumSteps;

		float MinScreenRadius;  //
		float AORadius;
		float GIRadius;
		float EffectRadius;
		float Thickness;  //
		float2 DepthFadeRange;
		float DepthFadeScaleConst;

		float GISaturation;  //
		float GIDistanceCompensation;
		float GICompensationMaxDist;
		float pad1;

		float AOPower;  //
		float GIStrength;

		float DepthDisocclusion;
		float NormalDisocclusion;
		uint MaxAccumFrames;  //

		float BlurRadius;
		float DistanceNormalisation;

		float2 pad;
	};
	STATIC_ASSERT_ALIGNAS_16(SSGICB);
	eastl::unique_ptr<ConstantBuffer> ssgiCB;

	/**
	 * @brief This frame's constants for textures of a_texDim, once per frame on either path: it also keeps the camera for the
	 * next frame's reprojection.
	 */
	SSGICB BuildConstants(float2 a_texDim);
	float4x4 prevInvView = {};

	eastl::unique_ptr<Texture2D> texNoise = nullptr;
	eastl::unique_ptr<Texture2D> texWorkingDepth = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavWorkingDepth[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texPrevGeo = nullptr;
	eastl::unique_ptr<Texture2D> texRadiance = nullptr;
	eastl::unique_ptr<Texture2D> texRadianceTemp = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavRadiance[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texNormal = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavNormal[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texAccumFrames[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texAo[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texIlY[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texIlCoCg[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texGiSpecular[2] = { nullptr };

	/** @brief Returns the current output SRVs for AO, indirect lighting Y/CoCg, and specular GI (or nullptrs if disabled). */
	std::tuple<ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*> GetOutputTextures();

	winrt::com_ptr<ID3D11SamplerState> linearClampSampler = nullptr;
	winrt::com_ptr<ID3D11SamplerState> pointClampSampler = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> prefilterDepthsCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prefilterRadianceCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prefilterNormalCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> radianceDisoccCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> giCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> blurCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> upsampleCompute = nullptr;
};

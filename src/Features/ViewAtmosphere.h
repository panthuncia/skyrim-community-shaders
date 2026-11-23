#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "Utils/Setting.h"
#include "Utils/TexturePreview.h"

#include <array>
#include <cstdint>

class ViewAtmosphere : public Feature
{
public:
	/** @brief Binds the toggle and the grid settings to their frame variables. The global is the only instance. */
	ViewAtmosphere();

	std::string GetName() override { return "Atmosphere and Fog"; }
	std::string GetShortName() override { return "ViewAtmosphere"; }
	std::string GetDisplayName() override { return T("feature.view_atmosphere.name", "Atmosphere and Fog"); }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	bool IsCore() const override { return true; }
	bool IsAlwaysEnabled() const override { return true; }
	/** @brief Describes the shared volume and its quality controls. */
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	/** @brief Draws shared quality controls. */
	void DrawSettings() override;
	bool HasDebugSection() const override { return true; }
	/** @brief Draws the shared volume previews. */
	void DrawDebugSection() override;
	/** @brief Loads global volume quality settings. */
	void LoadSettings(json& value) override;
	/** @brief Saves global volume quality settings. */
	void SaveSettings(json& value) override;
	/** @brief Restores shared volume quality defaults. */
	void RestoreDefaultSettings() override;
	/** @brief Sets up the render graph path in the graph modes. */
	void SetupResources() override;
	/** @brief The settings block the shaders read (the FeatureData copy). Its layout is fixed. */
	struct alignas(16) GpuSettings
	{
		uint enabled = 1;
		uint nearPixelSize = 16;
		uint nearSlices = 64;
		uint middlePixelSize = 64;
		uint middleSlices = 32;
		uint distantPixelSize = 32;
		uint distantSlices = 64;
		uint historyMissSamples = 4;
		float nearRange = 8000.0f;
		float localRange = 60000.0f;
		float historyWeight = 0.96f;
		float sampleJitter = 0.0f;
		float shadowBias = 0.002f;
		float depthDistribution = 8.0f;
		float lookupJitter = 1.0f;
		float padding = 0.0f;
	};
	STATIC_ASSERT_ALIGNAS_16(GpuSettings);
	static_assert(sizeof(GpuSettings) == 64);

	/** @brief The CPU settings. The toggle and the grid sizes change the frame's shape, so they are CS::Setting. */
	struct Settings
	{
		CS::Setting<std::uint32_t> enabled = 1;
		CS::Setting<std::uint32_t> nearPixelSize = 16;
		CS::Setting<std::uint32_t> nearSlices = 64;
		CS::Setting<std::uint32_t> middlePixelSize = 64;
		CS::Setting<std::uint32_t> middleSlices = 32;
		CS::Setting<std::uint32_t> distantPixelSize = 32;
		CS::Setting<std::uint32_t> distantSlices = 64;
		uint historyMissSamples = 4;
		float nearRange = 8000.0f;
		float localRange = 60000.0f;
		float historyWeight = 0.96f;
		float sampleJitter = 0.0f;
		float shadowBias = 0.002f;
		float depthDistribution = 8.0f;
		float lookupJitter = 1.0f;
	} settings;

	/** @brief The GPU block built from the settings. */
	GpuSettings GetCommonBufferData() const;

	/** @brief Where the three grids sit in the atlas. */
	struct AtlasLayout
	{
		DirectX::XMUINT4 size[3];    // per grid: width, height, slices
		DirectX::XMUINT4 offset[3];  // per grid: its corner in the atlas
		DirectX::XMUINT4 atlas;      // the atlas extent
	};

	/**
	* @brief The grids and the atlas for a screen size and the grid settings, at most 8 M texels. Clamps the settings as
	* Prepare does. D3D11 passes the dynamic size, the graph the display size.
	*/
	static AtlasLayout LayoutFor(float2 a_screen, std::uint32_t a_nearPixels, std::uint32_t a_middlePixels, std::uint32_t a_distantPixels,
		std::uint32_t a_nearSlices, std::uint32_t a_middleSlices, std::uint32_t a_distantSlices);

	/** @brief One grid of the parameters block (Common/ViewAtmosphere.hlsli). */
	struct Grid
	{
		DirectX::XMUINT4 size;
		DirectX::XMUINT4 offset;
		float4 depth;
	};
	/** @brief The parameters block the shaders read (ViewAtmosphere::Data). All zero means no medium. */
	struct Parameters
	{
		Grid grids[3];
		DirectX::XMUINT4 atlas;
		DirectX::XMUINT4 flags;
		float4 history;
		float4x4 clipToWorld;
	};
	STATIC_ASSERT_ALIGNAS_16(Parameters);
	static_assert(sizeof(Grid) == 48 && sizeof(Parameters) == 256);

	/** @brief The inputs the inject pass reads this frame: the bits of flags.z. */
	struct InputFlags
	{
		bool directionalShadow = false;  // 1: the shadow map and its light data
		bool ibl = false;                // 4
		bool probes = false;             // 8: Skylighting's probes
		bool localLights = false;        // 32: Light Limit Fix's lists
		bool localShadows = false;       // 64: its local shadow cache
		bool engineShadows = false;      // 128: the engine's local shadow maps
		bool terrain = false;            // 256: Terrain Shadows' heights
		bool cloudLayer = false;         // 512: Cloud Shadows' last layer
		bool environment = false;        // 1024: Dynamic Cubemaps' environment

		std::uint32_t Bits() const;
	};
	/** @brief The flags.z bits whose change resets the history: the directional shadow, terrain and cloud layer. */
	static constexpr std::uint32_t kShadowInputBits = 1u | 256u | 512u;

	/**
	* @brief The parameters both paths compute, as Prepare does: the grids and the atlas of a_layout, flags.x and flags.y (the fog),
	* the history weights and the unjittered clip-to-world. flags.z is left zero. a_continuous says the history holds the previous
	* frame of the same grids; without it the history weight is zero.
	*/
	Parameters ParametersFor(const AtlasLayout& a_layout, bool a_useFog, bool a_continuous) const;

	/** @brief The inject pass's samplers: linear clamp (s0), or the shadow map's comparison sampler (s1). */
	static D3D11_SAMPLER_DESC SamplerDesc(bool a_comparison);

	/** @brief Prepares the shared volume before standalone fog selects its path. */
	bool Prepare();
	/** @brief Reports whether post-water composition is available for the active media. */
	bool CompositeEnabled();
	/** @brief Composes the finished medium once after water and before transparency. */
	void CompositeAfterWater();
	/** @brief Prepares engine local-light shadow bindings for either volume renderer. */
	std::pair<ID3D11ShaderResourceView*, ID3D11Buffer*> PrepareLocalShadows(bool enabled);
	/** @brief Injects and integrates the prepared volume after feature prepasses. */
	void Render();
	/**
	* @brief Builds and binds a small omnidirectional medium before the six reflection faces. In the graph modes it only notes what
	* ORGViewAtmosphereReflection's passes read, for the next revision.
	*/
	void RenderReflection();
	/** @brief What the reflection medium reads this frame, for its parameters' flags. */
	struct ReflectionFlags
	{
		bool fog = false;         // Exponential Height Fog is active (flags.y)
		bool ibl = false;         // IBL's environment SH is bound (flags.z 4)
		bool probes = false;      // Skylighting's probes are bound (flags.z 8)
		bool terrain = false;     // Terrain Shadows' heights are ready (flags.z 256)
		bool cloudLayer = false;  // Cloud Shadows' last layer is bound (flags.z 512)
	};
	/** @brief The reflection medium's parameters (the 256 bytes Reflection.cs.hlsl reads), with flags.w = 1. D3D11 and the graph share it. */
	static std::array<std::uint32_t, 64> ReflectionParameters(const ReflectionFlags& a_flags);
	/** @brief Binds the integrated medium to a shader stage. */
	void Bind(bool compute = false);
	/** @brief Removes shared medium bindings from both shader stages. */
	void Unbind();
	/** @brief Invalidates shared medium shaders and history. */
	void ClearShaderCache() override;
	/** @brief Whether the shared volume replaces the fog's volumes and the AP shadow this frame. In the graph modes: the adopted revision runs it. */
	bool IsPrepared() const { return prepared; }
	/** @brief Whether the medium is rendered this frame. In the graph modes it is IsPrepared. */
	bool IsReady() const { return ready; }
	ID3D11ShaderResourceView* Luminance() const { return integratedLum ? integratedLum->srv.get() : nullptr; }
	ID3D11ShaderResourceView* Transmittance() const { return integratedTr ? integratedTr->srv.get() : nullptr; }
	ID3D11ShaderResourceView* Source() const { return ready && history ? history->srv.get() : nullptr; }
	/** @brief Cloud/clear diffuse transport from the last completed reflection sky update. */
	ID3D11ShaderResourceView* CloudDiffuseTransport() const { return ready && settings.enabled && reflectionSkySH ? reflectionSkySH->srv.get() : nullptr; }

private:
	TexturePreview preview;
	Parameters data{};
	std::unique_ptr<StructuredBuffer> parameters;
	std::unique_ptr<ConstantBuffer> dispatchCB;
	std::unique_ptr<ConstantBuffer> engineLocalShadowCB;
	std::unique_ptr<Texture3D> source, history, integratedLum, integratedTr;
	winrt::com_ptr<ID3D11ComputeShader> injectCS, integrateCS;
	winrt::com_ptr<ID3D11SamplerState> linearSampler, shadowSampler;
	winrt::com_ptr<ID3D11ComputeShader> mediumCompositeCS;
	std::unique_ptr<StructuredBuffer> reflectionParameters;
	std::unique_ptr<Texture3D> reflectionLum, reflectionTr;
	std::unique_ptr<Texture2D> reflectionSkySH;
	winrt::com_ptr<ID3D11ComputeShader> reflectionCS, reflectionSkyCS;
	winrt::com_ptr<ID3D11SamplerState> reflectionSampler;
	bool reflectionShadersRequested = false;
	uint32_t mediumCompositeFrame = UINT32_MAX;
	bool mediumShaderRequested = false;
	bool prepared = false;
	bool ready = false;
	uint32_t lastFrame = UINT32_MAX;
};

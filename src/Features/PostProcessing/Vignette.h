#pragma once

#include "Buffer.h"
#include "PostProcessFeature.h"

struct Vignette : public PostProcessFeature
{
	virtual inline std::string GetType() const override { return "Vignette"; }
	virtual inline std::string GetDisplayName() const override { return T("feature.post_processing.vignette.name", "Vignette"); }
	virtual inline std::string GetDesc() const override { return T("feature.post_processing.vignette.description", "Simulates natural vignetting caused by angled rays hitting the film or sensor array."); }

	struct Settings
	{
		float FocalLength = 1.f;
		float Anamorphism = 1.f;
		float Power = 3.f;
	} settings;

	struct alignas(16) VignetteCB
	{
		Settings settings;
		float AspectRatio;

		float2 RcpDynRes;
		float pad[2];
	};
	eastl::unique_ptr<ConstantBuffer> vignetteCB = nullptr;

	eastl::unique_ptr<Texture2D> texOutput = nullptr;

	winrt::com_ptr<ID3D11PixelShader> vignettePS = nullptr;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileRasterShaders();

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json&) override;
	virtual void SaveSettings(json&) override;

	virtual void DrawSettings() override;

	virtual void Draw(TextureInfo&) override;

	/**
	 * @brief This frame's constants for a pass of a_res pixels: the settings, with the focal length from Cinematic Camera
	 * while it drives the lens. Shared by the D3D11 draw and the ORG chain.
	 */
	VignetteCB Constants(float2 a_res) const;
};

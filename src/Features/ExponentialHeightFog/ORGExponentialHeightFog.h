#pragma once

#include <d3d11.h>
#include <memory>
#include <string_view>

/**
 * @brief Exponential Height Fog's volumetric fog on the render graph (CS_ORG_EXPONENTIAL_HEIGHT_FOG=1).
 *
 * The D3D11 path's work, as passes at the Prepass anchor, which runs right after the features' Prepass, where the D3D11 path
 * dispatches. For each volume (near, and far when it covers any depth): conservative depth, material, light scattering and
 * integration compute passes, then copies of the scattering and the conservative depth into their histories. A copy pass
 * first fills the directional shadow light buffer from the latch. With the far volume off, a clear fills its integrated
 * volume as the D3D11 path's clear does.
 *
 * Resources: the per-frame volumes are transient graph textures (aliasable); the histories have History lifetime; the two
 * integrated volumes are exported as D3D11 SRVs, which the engine's pixel shaders read at t19 and t22. They are also
 * published under their names (PublishedResources), and Drawcall Limit Fix's frame textures at t19 and t22 read them through
 * the graph while NearSRV and FarSRV answer. The per-frame constants (VolumetricFogCB, the engine's local shadows, the directional shadow
 * light) are one latch region. The shared constant buffers come from SharedConstants.
 *
 * Inputs are read as graph resources: the features already on the graph by their resource names (IBL's SH textures,
 * Skylighting's probes, Terrain Shadows' heights, Light Limit Fix's lights, lists and local shadows), and the engine's and
 * D3D11 features' textures through FrameAnchors imports (scene depth, shadow maps, Cloud Shadows, a D3D11 IBL or Terrain
 * Shadows). A frame whose inputs the graph cannot read (Light Limit Fix or Skylighting still on D3D11, a missing Cloud or
 * Terrain Shadows texture) runs on the D3D11 path instead; a failed program or graph returns the feature to D3D11 for good.
 *
 * Render thread throughout. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGExponentialHeightFog
{
public:
	/** @brief The integrated volumes' published ids (PublishedResources), their twins' names. */
	static constexpr std::string_view kNearIntegratedId = "ExponentialHeightFog::IntegratedLightScattering";
	static constexpr std::string_view kFarIntegratedId = "ExponentialHeightFog::FarIntegratedLightScattering";

	/** @brief Whether CS_ORG_EXPONENTIAL_HEIGHT_FOG=1 is set. */
	static bool Requested();

	static ORGExponentialHeightFog& Get();

	/** @brief Reserves the Prepass anchor when requested (the feature's PostPostLoad). */
	static void Reserve();

	/**
	 * @brief At the feature's Prepass, with the volumetric fog on and dense: this frame's volumes on the render graph, which the
	 * Prepass anchor's epoch then runs. @return Whether the graph has the frame; false: the D3D11 path runs it.
	 */
	bool Prepass();

	/** @brief The exported integrated volumes (t19, t22) of the last frame the graph ran; null when the D3D11 path has them. */
	ID3D11ShaderResourceView* NearSRV() const;
	ID3D11ShaderResourceView* FarSRV() const;

	/** @brief Drops the graph path's client and resources (the volumetric fog turned off). Its next Prepass makes them again. */
	void Release();

	~ORGExponentialHeightFog();

private:
	ORGExponentialHeightFog() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
	bool failed = false;
};

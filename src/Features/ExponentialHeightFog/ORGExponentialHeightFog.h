#pragma once

#include <d3d11.h>
#include <functional>
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
 * The dispatches go through a run gate: a run without the fog's frame (the D3D11 path has it, or another client keeps the
 * anchor going) dispatches no groups. The copies and the clear have no indirect form and run on every run of the anchor. On
 * such a run they copy or clear what no reader uses: the next frame on the graph is not consecutive, so it reads no history.
 *
 * Resources: the per-frame volumes are transient graph textures (aliasable); the histories have History lifetime; the two
 * integrated volumes are exported as D3D11 SRVs, which the engine's pixel shaders read at t19 and t22. They are also
 * published under their names (PublishedResources), and Drawcall Limit Fix's frame textures at t19 and t22 read them through
 * the graph while NearSRV and FarSRV answer. The per-frame constants (VolumetricFogCB, the engine's local shadows, the directional shadow
 * light) are one latch region. The shared constant buffers come from SharedConstants.
 *
 * Inputs are read as graph resources. A producer's texture is read by its published id while the view its feature binds is
 * that id's export (IBL's SH textures, Skylighting's probes, Terrain Shadows' heights, Cloud Shadows' occlusion, Terrain
 * Blending's depth, Light Limit Fix's lights, lists and local shadows); otherwise the D3D11 texture behind the view is imported
 * (FrameAnchors). The engine's targets (scene depth, shadow maps) are its imports. Without the sky the shader reads no terrain
 * or cloud shadow, so those inputs are left out then. A frame whose inputs the graph cannot read (Light Limit Fix or
 * Skylighting still on D3D11, a missing Cloud or Terrain Shadows texture) runs on the D3D11 path instead; a failed program or
 * graph returns the feature to D3D11 for good.
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

	/** @brief Before the render graph initializes (the feature's PostPostLoad): reserves the Prepass anchor when requested. */
	static void Reserve();

	/**
	 * @brief At SetupResources, with the runtime active: makes the programs, samplers, latch and run gate. The client is
	 * registered at the first Prepass, when the grid sizes and inputs are known, and again when they change.
	 * @param a_onFailed Called once if the graph fails later; the feature returns to D3D11.
	 * @return Whether the volumetric fog can run on the render graph; false keeps it on D3D11.
	 */
	bool Setup(std::function<void()> a_onFailed);

	/** @brief Whether the render-graph path is set up and the runtime is still up. */
	bool IsActive() const;

	/**
	 * @brief At the feature's Prepass, with the volumetric fog on and dense: this frame's volumes on the render graph, which the
	 * Prepass anchor's epoch then runs.
	 * @return Whether the graph has the frame; false: the D3D11 path runs it.
	 */
	bool Prepass();

	/** @brief The exported integrated volumes (t19, t22) of the last frame the graph ran; null when the D3D11 path has them. */
	ID3D11ShaderResourceView* NearSRV() const;
	ID3D11ShaderResourceView* FarSRV() const;

	/** @brief Drops the client and the volumes (the volumetric fog turned off). The programs stay; the next Prepass registers again. */
	void Release();

	~ORGExponentialHeightFog();

private:
	ORGExponentialHeightFog() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
	bool failed = false;
};

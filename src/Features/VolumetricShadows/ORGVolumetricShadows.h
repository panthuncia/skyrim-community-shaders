#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

/**
 * @brief Volumetric Shadows' shadow map downsample and blur as render-graph passes (CS_ORG_VOLUMETRIC_SHADOWS=1).
 *
 * The D3D11 path runs at the engine's sun shadow mask draw (State::Draw, the RenderShadowmask Utility pass). That draw only
 * marks the point where the inputs are bound. The inputs are the sun cascades (kSHADOWMAPS_ESRAM, the mask's t4) and the
 * volumetric lighting copy (kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM). Both are drawn inside Main::RenderShadowMaps, DCLF's
 * shadow epoch included. So they are final at the EarlyPrepass anchor, and the passes run there instead of at a new anchor.
 *
 * The passes are 2 downsamples (one per cascade, into the two mips of the shared shadow map) and 4 blurs, each through a
 * transient, aliasable temporary. The shared shadow map is exported as a D3D11 SRV: engine draws read it at t18. The shadow mask
 * draw still binds it at t18, as the D3D11 path does. It is also published (PublishedResources): DCLF's frame texture at t18
 * reads it through the graph.
 *
 * Everything here runs on the render thread. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGVolumetricShadows
{
public:
	/** @brief The shared shadow map's published id (PublishedResources), its twin's name. */
	static constexpr std::string_view kShadowCopyId = "VolumetricShadows::ShadowCopy";

	/** @brief Whether CS_ORG_VOLUMETRIC_SHADOWS=1 is set. */
	static bool Requested();

	/** @brief Reserves the EarlyPrepass anchor when requested. Call before the render graph initializes (PostPostLoad). */
	static void Reserve();

	static ORGVolumetricShadows& Get();

	/**
	 * @brief At SetupResources, with the runtime active: makes the programs, sampler and textures, registers the anchor client and
	 * publishes the shared shadow map.
	 * @param a_onFailed Called once if the graph fails later; the D3D11 dispatches run again.
	 * @return Whether the passes run on the render graph; false keeps the D3D11 dispatches.
	 */
	bool Setup(std::function<void()> a_onFailed);

	/**
	 * @brief The feature's EarlyPrepass, before the anchor runs: imports the engine's shadow maps again when the engine replaced
	 * them. The passes are left out of the graph while the cascades are missing.
	 */
	void UpdateImports();

	/** @brief Whether the passes run on the render graph (set up, and the runtime still up); false keeps the D3D11 dispatches. */
	bool IsActive() const;

	/**
	 * @brief At the sun's shadow mask draw: checks once that the engine's t4 is the cascade map the passes read. A different
	 * view turns the ORG path off, so the frame falls back to the D3D11 dispatches.
	 */
	void CheckShadowMaskInput(ID3D11ShaderResourceView* a_shadowMap);

	/** @brief The shared shadow map's exported view (both mips); null when inactive. */
	ID3D11ShaderResourceView* SharedShadowMapSRV() const;

	/** @brief The exported texture, for the settings' buffer viewer; null when inactive. */
	ID3D11Texture2D* SharedShadowMapTexture() const;

	~ORGVolumetricShadows();

private:
	ORGVolumetricShadows() = default;

	/** @brief Leaves the ORG path for good: the client goes and the shared map is withdrawn. */
	void Stop();

	struct Impl;
	std::unique_ptr<Impl> impl;
};

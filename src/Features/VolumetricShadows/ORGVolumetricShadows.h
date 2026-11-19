#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

/**
* @brief Volumetric Shadows' shadow map downsample and blur on the render graph, with CS_ORG_VOLUMETRIC_SHADOWS=1.
*
* The D3D11 path runs at the sun shadow mask draw. Its inputs are drawn inside Main::RenderShadowMaps, so they are final at
* EarlyPrepass and the passes run there. Two downsamples and four blurs write the shared shadow map, which is exported for t18
* and published for DCLF. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGVolumetricShadows
{
public:
	/** @brief The shared shadow map's published id. */
	static constexpr std::string_view kShadowCopyId = "VolumetricShadows::ShadowCopy";

	static bool Requested();

	/** @brief Call in PostPostLoad, before the render graph initializes. */
	static void Reserve();

	static ORGVolumetricShadows& Get();

	/** @brief At SetupResources with the runtime active. @return False keeps the D3D11 dispatches. */
	bool Setup();

	/** @brief Before the anchor runs. Re-imports replaced shadow maps. The passes are left out while the cascades are missing. */
	void UpdateImports();

	/** @brief Set up, and the runtime still up. */
	bool IsActive() const;

	/** @brief Checks once that the engine's t4 is the cascade map the passes read. A different view turns the ORG path off. */
	void CheckShadowMaskInput(ID3D11ShaderResourceView* a_shadowMap);

	/** @brief Both mips. Null when inactive. */
	ID3D11ShaderResourceView* SharedShadowMapSRV() const;

	/** @brief For the settings' buffer viewer. Null when inactive. */
	ID3D11Texture2D* SharedShadowMapTexture() const;

	~ORGVolumetricShadows();

private:
	ORGVolumetricShadows() = default;

	/** @brief For good. */
	void Stop();

	struct Impl;
	std::unique_ptr<Impl> impl;
};

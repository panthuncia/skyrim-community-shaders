#pragma once

#include "RenderGraph/Port.h"

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

/**
* @brief Volumetric Shadows' shadow map downsample and blur on the render graph, in graph mode.
*
* The D3D11 path runs at the sun shadow mask draw. Its inputs are drawn inside Main::RenderShadowMaps, so they are final at
* EarlyPrepass and the passes run there. Two downsamples and four blurs write the shared shadow map, which is exported for t18
* and published for DCLF. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGVolumetricShadows : public RenderGraph::Port<ORGVolumetricShadows>
{
public:
	static constexpr std::string_view kName = "Volumetric Shadows";

	/** @brief The shared shadow map's published id. */
	static constexpr std::string_view kShadowCopyId = "VolumetricShadows::ShadowCopy";

	/** @brief At SetupResources. Outside graph mode it does nothing; in graph mode a failure is fatal. */
	void Setup();

	/**
	* @brief Before the anchor runs. Fatal when the cascades go away once the client imported them. The client follows the
	* ShadowCascades frame variable, so nothing here registers it.
	*/
	void UpdateImports();

	/** @brief Checks once that the engine's t4 is the cascade map the passes read. A different view is fatal. */
	void CheckShadowMaskInput(ID3D11ShaderResourceView* a_shadowMap);

	/** @brief Both mips. Null when inactive. */
	ID3D11ShaderResourceView* SharedShadowMapSRV() const;

	/** @brief For the settings' buffer viewer. Null when inactive. */
	ID3D11Texture2D* SharedShadowMapTexture() const;
	~ORGVolumetricShadows();

private:
	friend class RenderGraph::Port<ORGVolumetricShadows>;
	ORGVolumetricShadows();

	struct Impl;
	std::unique_ptr<Impl> impl;
};

#pragma once

#include "RenderGraph/Port.h"

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

/**
* @brief Screen Space Shadows on the render graph, in graph mode.
*
* At the Prepass anchor, a clear to white then one raymarch pass. Bend's dispatch list changes every frame but the recording
* is kept, so the pass records the list's full capacity. Each dispatch reads constants from the latch and counts from the gate,
* and unused entries stay closed. The output is exported for the engine's t45 and published for DCLF.
* Any failure in graph mode is fatal. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGScreenSpaceShadows : public RenderGraph::Port<ORGScreenSpaceShadows>
{
public:
	static constexpr std::string_view kName = "Screen Space Shadows";

	/** @brief The shadow texture's published id. */
	static constexpr std::string_view kShadowTextureId = "ScreenSpaceShadows::ShadowTexture";

	/** @brief At SetupResources, once a session. Outside graph mode it does nothing; in graph mode a failure is fatal. */
	void Setup(const D3D11_TEXTURE2D_DESC& a_desc);

	/** @brief At the feature's Prepass. @return The SRV for the engine's t45, or null on the D3D11 path. */
	ID3D11ShaderResourceView* Prepass();

	/** @brief Null on the D3D11 path. */
	ID3D11ShaderResourceView* ShadowSRV() const;
	~ORGScreenSpaceShadows();

private:
	friend class RenderGraph::Port<ORGScreenSpaceShadows>;
	ORGScreenSpaceShadows();

	struct Impl;
	std::unique_ptr<Impl> impl;
};

#pragma once

struct ID3D11ShaderResourceView;

namespace RE
{
	class BSRenderPass;
}

namespace VanillaBinding
{
	void BindSharedConstantBuffers();
	void BindSceneDepth();

	/**
	 * @brief The scene depth view BindSceneDepth last bound at PS t17 (Util::GetCurrentSceneDepthSRV(true) then). Render thread.
	 * Graph passes that read what t17 holds take it from here, not from the D3D11 context.
	 */
	ID3D11ShaderResourceView* LastSceneDepth();
	void BindSkyResources(const RE::BSRenderPass* a_pass);
	void BindDrawResources();
}

#pragma once

#include <rhi.h>

#include <d3d11.h>

#include <span>
#include <string>

/** @brief Pure conversions from D3D11 pipeline state descriptions to BasicRHI. Values with no rhi equivalent are reported, not dropped. */
namespace RenderGraph::Engine::Pipeline
{
	/**
	* @brief Without independent blending every attachment copies target 0.
	* @return False when a factor or op is not a D3D11 value. a_error then says which.
	*/
	bool BlendFromD3D11(const D3D11_BLEND_DESC& a_desc, rhi::BlendState& a_out, std::string& a_error);

	bool ReadsBlendFactor(const D3D11_BLEND_DESC& a_desc);

	bool UsesDualSource(const D3D11_BLEND_DESC& a_desc);

	/** @brief The stencil reference is command list state and is not included. */
	rhi::DepthStencilState DepthStencilFromD3D11(const D3D11_DEPTH_STENCIL_DESC& a_desc);

	/** @brief ScissorEnable has no rhi field, so a pass uses a scissor covering the target. MultisampleEnable and AntialiasedLineEnable are dropped. */
	rhi::RasterState RasterFromD3D11(const D3D11_RASTERIZER_DESC& a_desc);
}


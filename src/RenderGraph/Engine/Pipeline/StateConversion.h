#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <Render/PipelineDesc.h>
#	include <rhi.h>

#	include <d3d11.h>

#	include <span>
#	include <string>

/** @brief Pure conversions from D3D11 pipeline state descriptions to BasicRHI and ORG. Values with no rhi equivalent are reported, not dropped. */
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

	/** @brief Unknown for patch lists and adjacency, which the engine does not use. */
	rhi::PrimitiveTopology TopologyFromD3D11(D3D11_PRIMITIVE_TOPOLOGY a_topology);

	/**
	* @brief One binding per input slot. Strides are 0 and come from the vertex buffers at draw time.
	* @param a_error Set for a format BasicRHI cannot express. That attribute is left out.
	*/
	org::PipelineInputLayout InputLayoutFromD3D11(std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements, std::string& a_error);

	/** @brief A stride of 0 leaves a binding as it is. */
	org::PipelineInputLayout WithStrides(org::PipelineInputLayout a_layout, std::span<const std::uint32_t> a_strides);
}

#endif

#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <Render/PipelineDesc.h>
#	include <rhi.h>

#	include <d3d11.h>

#	include <span>
#	include <string>

/**
 * @brief D3D11 pipeline state descriptions as BasicRHI and ORG describe them (P4.6).
 *
 * Pure functions, for the engine's state objects and for any port that still has a D3D11 description. Samplers are converted by
 * ShaderBindings::SamplerFromD3D11. Every D3D11 value the engine can pass has an rhi value since Phase 0 (stencil, blend factor,
 * SrcAlphaSat, dual source, depth clip). What has none is reported, not dropped silently.
 */
namespace RenderGraph::Engine::Pipeline
{
	/**
	 * @brief The rhi blend state of a D3D11 one. Every attachment is filled; without independent blending they copy target 0.
	 * @param a_error Set when a factor or op is not a D3D11 value; the result is then unusable.
	 * @return false on an error.
	 */
	bool BlendFromD3D11(const D3D11_BLEND_DESC& a_desc, rhi::BlendState& a_out, std::string& a_error);

	/** @brief Whether any enabled target reads the blend factor (BLEND_FACTOR / INV_BLEND_FACTOR). */
	bool ReadsBlendFactor(const D3D11_BLEND_DESC& a_desc);

	/** @brief Whether any enabled target reads the pixel shader's second output (the SRC1 factors). */
	bool UsesDualSource(const D3D11_BLEND_DESC& a_desc);

	/** @brief The rhi depth-stencil state of a D3D11 one, stencil included (the reference is command list state). */
	rhi::DepthStencilState DepthStencilFromD3D11(const D3D11_DEPTH_STENCIL_DESC& a_desc);

	/**
	 * @brief The rhi rasterizer state of a D3D11 one. ScissorEnable has no rhi field: a pass draws a scissor-off state with a
	 * scissor that covers the target. MultisampleEnable and AntialiasedLineEnable are not carried.
	 */
	rhi::RasterState RasterFromD3D11(const D3D11_RASTERIZER_DESC& a_desc);

	/** @brief The rhi topology of a D3D11 one; Unknown for patch lists and adjacency, which the engine does not use. */
	rhi::PrimitiveTopology TopologyFromD3D11(D3D11_PRIMITIVE_TOPOLOGY a_topology);

	/**
	 * @brief An ORG input layout from D3D11 elements: one binding per input slot (per vertex or per instance with its step rate),
	 * semantics copied. Strides are 0: they come from the vertex buffers at draw time (WithStrides).
	 * @param a_error Set for a format BasicRHI has no value for; that attribute is then left out.
	 */
	org::PipelineInputLayout InputLayoutFromD3D11(std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements, std::string& a_error);

	/** @brief a_layout with its bindings' strides set: a_strides[slot], 0 leaves a binding as it is. */
	org::PipelineInputLayout WithStrides(org::PipelineInputLayout a_layout, std::span<const std::uint32_t> a_strides);
}

#endif

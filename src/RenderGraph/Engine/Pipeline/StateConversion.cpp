#include "RenderGraph/Engine/Pipeline/StateConversion.h"

#if defined(CS_HAS_RENDER_GRAPH)

#	include <rhi_helpers.h>

#	include <algorithm>
#	include <format>
#	include <initializer_list>

namespace RenderGraph::Engine::Pipeline
{
	namespace
	{
		bool BlendFactorOf(D3D11_BLEND a_blend, rhi::BlendFactor& a_out)
		{
			switch (a_blend) {
			case D3D11_BLEND_ZERO:
				a_out = rhi::BlendFactor::Zero;
				return true;
			case D3D11_BLEND_ONE:
				a_out = rhi::BlendFactor::One;
				return true;
			case D3D11_BLEND_SRC_COLOR:
				a_out = rhi::BlendFactor::SrcColor;
				return true;
			case D3D11_BLEND_INV_SRC_COLOR:
				a_out = rhi::BlendFactor::InvSrcColor;
				return true;
			case D3D11_BLEND_SRC_ALPHA:
				a_out = rhi::BlendFactor::SrcAlpha;
				return true;
			case D3D11_BLEND_INV_SRC_ALPHA:
				a_out = rhi::BlendFactor::InvSrcAlpha;
				return true;
			case D3D11_BLEND_DEST_ALPHA:
				a_out = rhi::BlendFactor::DstAlpha;
				return true;
			case D3D11_BLEND_INV_DEST_ALPHA:
				a_out = rhi::BlendFactor::InvDstAlpha;
				return true;
			case D3D11_BLEND_DEST_COLOR:
				a_out = rhi::BlendFactor::DstColor;
				return true;
			case D3D11_BLEND_INV_DEST_COLOR:
				a_out = rhi::BlendFactor::InvDstColor;
				return true;
			case D3D11_BLEND_SRC_ALPHA_SAT:
				a_out = rhi::BlendFactor::SrcAlphaSat;
				return true;
			case D3D11_BLEND_BLEND_FACTOR:
				a_out = rhi::BlendFactor::BlendFactor;
				return true;
			case D3D11_BLEND_INV_BLEND_FACTOR:
				a_out = rhi::BlendFactor::InvBlendFactor;
				return true;
			case D3D11_BLEND_SRC1_COLOR:
				a_out = rhi::BlendFactor::Src1Color;
				return true;
			case D3D11_BLEND_INV_SRC1_COLOR:
				a_out = rhi::BlendFactor::InvSrc1Color;
				return true;
			case D3D11_BLEND_SRC1_ALPHA:
				a_out = rhi::BlendFactor::Src1Alpha;
				return true;
			case D3D11_BLEND_INV_SRC1_ALPHA:
				a_out = rhi::BlendFactor::InvSrc1Alpha;
				return true;
			default:
				return false;
			}
		}

		bool BlendOpOf(D3D11_BLEND_OP a_op, rhi::BlendOp& a_out)
		{
			// D3D11_BLEND_OP_ADD is 1; rhi::BlendOp::Add is 0, in the same order.
			if (a_op < D3D11_BLEND_OP_ADD || a_op > D3D11_BLEND_OP_MAX)
				return false;
			a_out = static_cast<rhi::BlendOp>(a_op - D3D11_BLEND_OP_ADD);
			return true;
		}

		// D3D11_COMPARISON_NEVER is 1; rhi::CompareOp::Never is 0, in the same order.
		rhi::CompareOp CompareOf(D3D11_COMPARISON_FUNC a_func)
		{
			return static_cast<rhi::CompareOp>(std::clamp<int>(a_func, D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS) - D3D11_COMPARISON_NEVER);
		}

		// D3D11_STENCIL_OP_KEEP is 1; rhi::StencilOp::Keep is 0, in the same order.
		rhi::StencilOp StencilOpOf(D3D11_STENCIL_OP a_op)
		{
			return static_cast<rhi::StencilOp>(std::clamp<int>(a_op, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_DECR) - D3D11_STENCIL_OP_KEEP);
		}

		rhi::StencilFaceState FaceOf(const D3D11_DEPTH_STENCILOP_DESC& a_face)
		{
			return { .failOp = StencilOpOf(a_face.StencilFailOp),
				.depthFailOp = StencilOpOf(a_face.StencilDepthFailOp),
				.passOp = StencilOpOf(a_face.StencilPassOp),
				.func = CompareOf(a_face.StencilFunc) };
		}

		bool Reads(const D3D11_BLEND_DESC& a_desc, std::initializer_list<D3D11_BLEND> a_factors)
		{
			const UINT targets = a_desc.IndependentBlendEnable ? 8u : 1u;
			for (UINT i = 0; i < targets; ++i) {
				const auto& target = a_desc.RenderTarget[i];
				if (!target.BlendEnable)
					continue;
				for (const auto factor : a_factors) {
					if (target.SrcBlend == factor || target.DestBlend == factor || target.SrcBlendAlpha == factor || target.DestBlendAlpha == factor)
						return true;
				}
			}
			return false;
		}
	}

	bool BlendFromD3D11(const D3D11_BLEND_DESC& a_desc, rhi::BlendState& a_out, std::string& a_error)
	{
		a_out = {};
		a_out.alphaToCoverage = a_desc.AlphaToCoverageEnable != FALSE;
		a_out.independentBlend = a_desc.IndependentBlendEnable != FALSE;
		a_out.numAttachments = 8;
		for (std::uint32_t i = 0; i < 8; ++i) {
			const auto& source = a_desc.RenderTarget[a_out.independentBlend ? i : 0];
			auto& target = a_out.attachments[i];
			target.enable = source.BlendEnable != FALSE;
			target.writeMask = static_cast<rhi::ColorWriteEnable>(source.RenderTargetWriteMask & 0xF);
			if (!BlendFactorOf(source.SrcBlend, target.srcColor) || !BlendFactorOf(source.DestBlend, target.dstColor) ||
				!BlendOpOf(source.BlendOp, target.colorOp) || !BlendFactorOf(source.SrcBlendAlpha, target.srcAlpha) ||
				!BlendFactorOf(source.DestBlendAlpha, target.dstAlpha) || !BlendOpOf(source.BlendOpAlpha, target.alphaOp)) {
				// A disabled target's factors are not read, but D3D11 still validates them, so a bad value is a real error.
				a_error = std::format("target {} has a blend factor or op that is not a D3D11 value", i);
				return false;
			}
		}
		return true;
	}

	bool ReadsBlendFactor(const D3D11_BLEND_DESC& a_desc)
	{
		return Reads(a_desc, { D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR });
	}

	bool UsesDualSource(const D3D11_BLEND_DESC& a_desc)
	{
		return Reads(a_desc, { D3D11_BLEND_SRC1_COLOR, D3D11_BLEND_INV_SRC1_COLOR, D3D11_BLEND_SRC1_ALPHA, D3D11_BLEND_INV_SRC1_ALPHA });
	}

	rhi::DepthStencilState DepthStencilFromD3D11(const D3D11_DEPTH_STENCIL_DESC& a_desc)
	{
		rhi::DepthStencilState out{};
		out.depthEnable = a_desc.DepthEnable != FALSE;
		out.depthWrite = a_desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
		out.depthFunc = CompareOf(a_desc.DepthFunc);
		out.stencilEnable = a_desc.StencilEnable != FALSE;
		out.stencilReadMask = a_desc.StencilReadMask;
		out.stencilWriteMask = a_desc.StencilWriteMask;
		out.front = FaceOf(a_desc.FrontFace);
		out.back = FaceOf(a_desc.BackFace);
		return out;
	}

	rhi::RasterState RasterFromD3D11(const D3D11_RASTERIZER_DESC& a_desc)
	{
		rhi::RasterState out{};
		out.fill = a_desc.FillMode == D3D11_FILL_WIREFRAME ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
		switch (a_desc.CullMode) {
		case D3D11_CULL_FRONT:
			out.cull = rhi::CullMode::Front;
			break;
		case D3D11_CULL_BACK:
			out.cull = rhi::CullMode::Back;
			break;
		default:
			out.cull = rhi::CullMode::None;
			break;
		}
		out.frontCCW = a_desc.FrontCounterClockwise != FALSE;
		out.depthBias = static_cast<float>(a_desc.DepthBias);
		out.depthBiasClamp = a_desc.DepthBiasClamp;
		out.slopeScaledDepthBias = a_desc.SlopeScaledDepthBias;
		out.depthClip = a_desc.DepthClipEnable != FALSE;
		return out;
	}

	rhi::PrimitiveTopology TopologyFromD3D11(D3D11_PRIMITIVE_TOPOLOGY a_topology)
	{
		switch (a_topology) {
		case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:
			return rhi::PrimitiveTopology::PointList;
		case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:
			return rhi::PrimitiveTopology::LineList;
		case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:
			return rhi::PrimitiveTopology::LineStrip;
		case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
			return rhi::PrimitiveTopology::TriangleList;
		case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
			return rhi::PrimitiveTopology::TriangleStrip;
		default:
			return rhi::PrimitiveTopology::Unknown;
		}
	}

	org::PipelineInputLayout InputLayoutFromD3D11(std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements, std::string& a_error)
	{
		org::PipelineInputLayout out;
		for (const auto& element : a_elements) {
			const auto format = rhi::helpers::ToRHI(element.Format);
			if (format == rhi::Format::Unknown) {
				a_error = std::format("{}{} has DXGI format {}, which BasicRHI has no value for", element.SemanticName ? element.SemanticName : "?", element.SemanticIndex,
					static_cast<int>(element.Format));
				continue;
			}
			const bool perInstance = element.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA;
			const auto binding = std::ranges::find(out.bindings, element.InputSlot, &rhi::InputBindingDesc::binding);
			if (binding == out.bindings.end()) {
				out.bindings.push_back({ .binding = element.InputSlot,
					.stride = 0,
					.rate = perInstance ? rhi::InputRate::PerInstance : rhi::InputRate::PerVertex,
					.instanceStepRate = perInstance ? (std::max)(1u, element.InstanceDataStepRate) : 1u });
			}
			// D3D11_APPEND_ALIGNED_ELEMENT and rhi::APPEND_ALIGNED are both 0xffffffff.
			out.attributes.push_back({ .binding = element.InputSlot,
				.offset = element.AlignedByteOffset,
				.format = format,
				.semanticName = element.SemanticName ? element.SemanticName : "",
				.semanticIndex = element.SemanticIndex });
		}
		return out;
	}

	org::PipelineInputLayout WithStrides(org::PipelineInputLayout a_layout, std::span<const std::uint32_t> a_strides)
	{
		for (auto& binding : a_layout.bindings) {
			if (binding.binding < a_strides.size() && a_strides[binding.binding])
				binding.stride = a_strides[binding.binding];
		}
		return a_layout;
	}
}

#endif

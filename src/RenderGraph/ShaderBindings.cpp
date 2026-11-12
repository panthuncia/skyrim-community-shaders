#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/ShaderBindings.h"

#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <OpenRenderGraph/Runtime.h>
#	include <Render/Runtime/DescriptorServiceAccess.h>
#	include <Render/Runtime/IDescriptorService.h>

#	include <algorithm>
#	include <exception>
#	include <iterator>
#	include <memory>

namespace ShaderBindings
{
	namespace
	{
		/**
		 * @brief The active service inside a graph callback, else the runtime's (setup, on the render thread); null without a host.
		 * The runtime's is the one every graph uses. It exists from the host's start, before any graph is built: with synchronous
		 * epochs no graph is built yet when the ports set up, and the first epoch's graph may be built last.
		 */
		org::runtime::IDescriptorService* Service(std::shared_ptr<org::runtime::IDescriptorService>& a_retained)
		{
			if (auto* service = org::runtime::GetActiveDescriptorService())
				return service;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return nullptr;
			if (host->Graph())
				a_retained = host->Graph()->RetainDescriptorService();
			if (!a_retained)
				a_retained = host->GetRuntime().Descriptors();
			return a_retained.get();
		}
	}

	rhi::SamplerDesc SamplerFromD3D11(const D3D11_SAMPLER_DESC& a_desc)
	{
		const auto address = [](D3D11_TEXTURE_ADDRESS_MODE a_mode) {
			switch (a_mode) {
			case D3D11_TEXTURE_ADDRESS_WRAP:
				return rhi::AddressMode::Wrap;
			case D3D11_TEXTURE_ADDRESS_MIRROR:
				return rhi::AddressMode::Mirror;
			case D3D11_TEXTURE_ADDRESS_BORDER:
				return rhi::AddressMode::Border;
			case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE:
				return rhi::AddressMode::MirrorOnce;
			default:
				return rhi::AddressMode::Clamp;
			}
		};
		rhi::SamplerDesc desc{};
		// D3D11_FILTER's bits: mip linear 0x1, mag linear 0x4, min linear 0x10, anisotropic 0x40, then the reduction in 0x180.
		const auto filter = static_cast<std::uint32_t>(a_desc.Filter);
		const bool anisotropic = (filter & 0x40) != 0;
		desc.mipFilter = (filter & 0x1) || anisotropic ? rhi::MipFilter::Linear : rhi::MipFilter::Nearest;
		desc.magFilter = (filter & 0x4) || anisotropic ? rhi::Filter::Linear : rhi::Filter::Nearest;
		desc.minFilter = (filter & 0x10) || anisotropic ? rhi::Filter::Linear : rhi::Filter::Nearest;
		desc.maxAnisotropy = anisotropic ? (std::max)(1u, static_cast<std::uint32_t>(a_desc.MaxAnisotropy)) : 1u;
		switch (filter & 0x180) {
		case 0x80:
			desc.compareEnable = true;
			desc.reduction = rhi::ReductionMode::Comparison;
			// D3D11_COMPARISON_NEVER is 1, rhi::CompareOp::Never 0, in the same order.
			desc.compareOp = static_cast<rhi::CompareOp>(std::clamp<int>(a_desc.ComparisonFunc, 1, 8) - 1);
			break;
		case 0x100:
			desc.reduction = rhi::ReductionMode::Min;
			break;
		case 0x180:
			desc.reduction = rhi::ReductionMode::Max;
			break;
		default:
			break;
		}
		desc.addressU = address(a_desc.AddressU);
		desc.addressV = address(a_desc.AddressV);
		desc.addressW = address(a_desc.AddressW);
		desc.mipLodBias = a_desc.MipLODBias;
		desc.minLod = a_desc.MinLOD;
		desc.maxLod = a_desc.MaxLOD;
		const auto border = [&](float a_r, float a_g, float a_b, float a_a) {
			return a_desc.BorderColor[0] == a_r && a_desc.BorderColor[1] == a_g && a_desc.BorderColor[2] == a_b && a_desc.BorderColor[3] == a_a;
		};
		if (border(0.0f, 0.0f, 0.0f, 0.0f))
			desc.borderPreset = rhi::BorderPreset::TransparentBlack;
		else if (border(0.0f, 0.0f, 0.0f, 1.0f))
			desc.borderPreset = rhi::BorderPreset::OpaqueBlack;
		else if (border(1.0f, 1.0f, 1.0f, 1.0f))
			desc.borderPreset = rhi::BorderPreset::OpaqueWhite;
		else
			desc.borderPreset = rhi::BorderPreset::Custom;
		std::copy(std::begin(a_desc.BorderColor), std::end(a_desc.BorderColor), desc.borderColor);
		return desc;
	}

	org::OwnedDescriptorBinding CreateSampler(const rhi::SamplerDesc& a_desc)
	{
		std::shared_ptr<org::runtime::IDescriptorService> retained;
		auto* service = Service(retained);
		if (!service) {
			logger::warn("[ORG] A sampler was asked for without a descriptor service (no graph host)");
			return {};
		}
		try {
			auto binding = service->CreateOwnedSampler(RenderGraphRuntime::Get().DeviceOwner(), a_desc);
			if (!binding)
				logger::error("[ORG] The backend could not create a sampler");
			return binding;
		} catch (const std::exception& e) {
			logger::error("[ORG] The backend could not create a sampler: {}", e.what());
			return {};
		}
	}

	org::OwnedDescriptorBinding CreateShaderResourceView(rhi::Resource a_resource, std::shared_ptr<const void> a_owner, const rhi::SrvDesc& a_desc)
	{
		std::shared_ptr<org::runtime::IDescriptorService> retained;
		auto* service = Service(retained);
		if (!service) {
			logger::warn("[ORG] A shader resource view was asked for without a descriptor service (no graph host)");
			return {};
		}
		try {
			auto binding = service->CreateOwnedShaderResourceView(RenderGraphRuntime::Get().DeviceOwner(), a_resource, std::move(a_owner), a_desc);
			if (!binding)
				logger::error("[ORG] The backend could not create a shader resource view");
			return binding;
		} catch (const std::exception& e) {
			logger::error("[ORG] The backend could not create a shader resource view: {}", e.what());
			return {};
		}
	}
}

#endif

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/ShaderBindings.h"

#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/Runtime/DescriptorServiceAccess.h>
#	include <Render/Runtime/IDescriptorService.h>

#	include <exception>
#	include <memory>

namespace ShaderBindings
{
	namespace
	{
		/** @brief The active service inside a graph callback, else the host graph's (setup, on the render thread); null without one. */
		org::runtime::IDescriptorService* Service(std::shared_ptr<org::runtime::IDescriptorService>& a_retained)
		{
			if (auto* service = org::runtime::GetActiveDescriptorService())
				return service;
			auto* host = RenderGraphRuntime::Get().Host();
			if (host && host->Graph())
				a_retained = host->Graph()->RetainDescriptorService();
			return a_retained.get();
		}
	}

	org::OwnedDescriptorBinding CreateSampler(const rhi::SamplerDesc& a_desc)
	{
		std::shared_ptr<org::runtime::IDescriptorService> retained;
		auto* service = Service(retained);
		if (!service) {
			logger::warn("[ORG] A sampler was asked for without a descriptor service");
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
			logger::warn("[ORG] A shader resource view was asked for without a descriptor service");
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

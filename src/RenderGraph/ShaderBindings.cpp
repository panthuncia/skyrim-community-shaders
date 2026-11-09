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
	org::OwnedDescriptorBinding CreateSampler(const rhi::SamplerDesc& a_desc)
	{
		// The active service inside a graph callback, else the host graph's (setup, on the render thread).
		std::shared_ptr<org::runtime::IDescriptorService> retained;
		auto* service = org::runtime::GetActiveDescriptorService();
		if (!service) {
			auto* host = RenderGraphRuntime::Get().Host();
			if (host && host->Graph()) {
				retained = host->Graph()->RetainDescriptorService();
				service = retained.get();
			}
		}
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
}

#endif

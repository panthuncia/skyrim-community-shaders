#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/Backend.h"

#	include "RenderGraph/Switches.h"

#	include <atomic>

namespace RenderGraph::Backend
{
	namespace
	{
		std::atomic<rhi::Backend> s_active{ rhi::Backend::Vulkan };
	}

	rhi::Backend Requested()
	{
		static const rhi::Backend requested = [] {
			const auto value = Switches::Value("CS_ORG_NATIVE_BACKEND");
			if (value.empty() || Switches::EqualsNoCase("CS_ORG_NATIVE_BACKEND", "vulkan"))
				return rhi::Backend::Vulkan;
			if (Switches::EqualsNoCase("CS_ORG_NATIVE_BACKEND", "d3d12"))
				return rhi::Backend::D3D12;
			logger::error("[ORG] CS_ORG_NATIVE_BACKEND={} is not a backend (vulkan or d3d12); the render graph stays on Vulkan", value);
			return rhi::Backend::Vulkan;
		}();
		return requested;
	}

	const char* Name(rhi::Backend a_backend)
	{
		return a_backend == rhi::Backend::D3D12 ? "D3D12" : "Vulkan";
	}

	void SetActive(rhi::Backend a_backend)
	{
		s_active.store(a_backend, std::memory_order_release);
		logger::info("[ORG] Render graph backend: {}", Name(a_backend));
	}

	rhi::Backend Active()
	{
		return s_active.load(std::memory_order_acquire);
	}

	bool D3D12Built()
	{
#	if defined(CS_ORG_HAS_D3D12)
		return true;
#	else
		return false;
#	endif
	}

	std::string RequestError()
	{
		if (Requested() != rhi::Backend::D3D12)
			return {};
		if (!D3D12Built())
			return "CS_ORG_NATIVE_BACKEND=d3d12, but this build has no D3D12 backend (built with CS_ORG_D3D12=OFF)";
		if (!Switches::On("CS_ORG_NATIVE"))
			return "CS_ORG_NATIVE_BACKEND=d3d12 needs CS_ORG_NATIVE=1: DXVK runs the game on Vulkan, so the render graph cannot share a D3D12 device with it";
		return {};
	}

	std::string UnsupportedReason(std::string_view a_what, std::string_view a_why)
	{
		if (!IsD3D12())
			return {};
		return fmt::format("{} is not available on D3D12 (CS_ORG_NATIVE_BACKEND=d3d12): {}", a_what, a_why);
	}
}

#endif

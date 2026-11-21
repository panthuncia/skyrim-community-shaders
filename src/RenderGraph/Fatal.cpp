#include "RenderGraph/Fatal.h"

#include "RenderGraph/RenderGraphRuntime.h"

namespace RenderGraph
{
	void Fatal(std::string_view a_area, std::string_view a_why)
	{
		logger::critical("[ORG] fatal: {}: {}", a_area, a_why);
		spdlog::default_logger()->flush();
		stl::report_and_fail(fmt::format("Community Shaders: {}: {}", a_area, a_why));
	}

	org::GraphHost& RequireGraphHost(std::string_view a_area)
	{
		auto& runtime = RenderGraphRuntime::Get();
		auto* host = runtime.Host();
		if (!host || !runtime.IsActive())
			Fatal(a_area, "the render graph is not running at its setup");
		return *host;
	}
}

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/FrameStaging.h"

#include "RenderGraph/Fatal.h"

#include <OpenRenderGraph/Runtime.h>

#include <algorithm>
#include <exception>
#include <format>

namespace RenderGraph
{
	Staging AcquireStaging(std::string_view a_area, std::uint64_t a_bytes)
	{
		auto* runtime = org::Runtime::Current();
		if (!runtime || !runtime->Arena())
			Fatal(a_area, "a staging region was asked for with no render graph runtime");
		try {
			return runtime->Arena()->AcquireOversize((std::max)(a_bytes, std::uint64_t{ 4 }));
		} catch (const std::exception& e) {
			Fatal(a_area, std::format("a staging region of {} bytes could not be made: {}", a_bytes, e.what()));
		}
	}
}


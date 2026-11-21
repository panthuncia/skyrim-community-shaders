// The depth pyramid's entry, setup and state.

#include "Features/DrawcallLimitFix/Core/Extensions/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "RenderGraph/Fatal.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written only during InstallFeatures, before DCLF's threads exist. Read-only after.
		DepthPyramidEntry resolved;
		bool registered = false;
	}

	void ResolveDepthPyramid(const Api::DepthPyramidContribution& a_registered)
	{
		static bool once = false;
		if (std::exchange(once, true))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the depth pyramid was resolved twice");
		if (!a_registered.setup)
			return;
		resolved = { std::string(a_registered.resource), std::string(a_registered.scratchResource), a_registered.setup };
		registered = true;
	}

	const DepthPyramidEntry* DepthPyramidOf() { return registered ? &resolved : nullptr; }

	std::shared_ptr<const void> SetUpDepthPyramid(rhi::Device a_device, std::uint32_t a_width, std::uint32_t a_height, Api::DepthPyramid& a_out)
	{
		if (!registered)
			return nullptr;
		return resolved.setup({ a_device, a_width, a_height }, a_out);
	}

	const std::shared_ptr<const void>& DepthPyramidState(const Api::PassContext& a_context) { return a_context.Main()->hzbState; }
}

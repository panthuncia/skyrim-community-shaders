#pragma once

// Core's side of Api/DepthPyramid.h: what the setup gets, the contribution's entry, and its state for the producer's pass.
// Without a contribution there is no pyramid and culling is frustum only.

#include "Features/DrawcallLimitFix/Api/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Api/Passes.h"

#include <rhi.h>

#include <cstdint>
#include <memory>
#include <string>

namespace DCLF::Api
{
	/** @brief What the pyramid's setup gets on the render thread: the device and the main view's extent. */
	class DepthPyramidSetup
	{
	public:
		rhi::Device device;
		std::uint32_t width = 0, height = 0;
	};
}

namespace DCLF::Extensions
{
	/** @brief The contribution's entry, with its names owned. */
	struct DepthPyramidEntry
	{
		std::string resource, scratchResource;
		decltype(Api::DepthPyramidContribution::setup) setup = nullptr;
	};

	/** @brief Main thread, once, at the end of InstallFeatures. */
	void ResolveDepthPyramid(const Api::DepthPyramidContribution& a_registered);
	/** @brief Any thread, after InstallFeatures. Null without a contribution. */
	const DepthPyramidEntry* DepthPyramidOf();
	/** @brief Render thread, per main resource set. Runs setup. Without a contribution, a_out stays empty and the state is null. */
	std::shared_ptr<const void> SetUpDepthPyramid(rhi::Device a_device, std::uint32_t a_width, std::uint32_t a_height, Api::DepthPyramid& a_out);
	/** @brief Render thread, at graph build. The state setup made for a_context's main resources, as the producer's type. */
	const std::shared_ptr<const void>& DepthPyramidState(const Api::PassContext& a_context);
	template <class T>
	std::shared_ptr<const T> DepthPyramidState(const Api::PassContext& a_context)
	{
		return std::static_pointer_cast<const T>(DepthPyramidState(a_context));
	}
}

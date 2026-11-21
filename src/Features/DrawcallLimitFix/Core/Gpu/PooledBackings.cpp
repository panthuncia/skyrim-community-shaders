// DCLF's settings for OMS's graph host backing pools.
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/ElasticCapacity.h>
#include <ORGModuleServices/GraphHostBackings.h>

#include <cstdlib>

namespace DCLF::PooledBackings
{
	namespace
	{
		const org::services::GraphHostAccess& Host()
		{
			static const org::services::GraphHostAccess host = []() -> const org::GraphHost* { return RenderGraphRuntime::Get().Host(); };
			return host;
		}
	}

	std::uint64_t DeviceAddressOf(const org::Buffer& a_buffer)
	{
		return org::services::GraphHostDeviceAddress(Host(), a_buffer);
	}

	org::services::BackingPoolHooks Hooks()
	{
		return org::services::GraphHostBackingHooks(Host());
	}

	std::uint32_t PerFrameSpares()
	{
		// The newest frame's backing is still bound when the next is acquired. Measured: one fewer remakes a backing every few frames.
		return org::services::PerFrameSpares(RenderGraphRuntime::kGameFramesInFlight);
	}

	std::uint32_t SceneCopies()
	{
		static const std::uint32_t copies = [] {
			const auto& value = SwitchValue(Switch::SceneCopies);
			const auto asked = value.empty() ? 3u : static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
			return org::services::PerFrameCopies(asked, RenderGraphRuntime::kGameFramesInFlight);
		}();
		return copies;
	}

	const org::services::ShrinkPolicy& Shrink()
	{
		static const org::services::ShrinkPolicy policy = [] {
			const auto& value = SwitchValue(Switch::Shrink);
			if (value == "off" || value == "0")
				return org::services::BackingShrinkPolicy(org::services::BackingShrink::Never);
			if (value == "eager")
				return org::services::BackingShrinkPolicy(org::services::BackingShrink::Eager);
			return org::services::BackingShrinkPolicy(org::services::BackingShrink::Settled);
		}();
		return policy;
	}
}

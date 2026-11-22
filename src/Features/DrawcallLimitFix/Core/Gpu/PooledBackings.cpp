// DCLF's settings for OMS's graph host backing pools.
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "RenderGraph/Relocation/Holders.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/ElasticCapacity.h>
#include <ORGModuleServices/GraphHostBackings.h>
#include <Resources/Buffers/Buffer.h>

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
		// The scene's set reads the address again after an idle move (SceneBufferSet follows the journal).
		::RenderGraph::Relocation::FollowAtIdle(a_buffer);
		return org::services::GraphHostDeviceAddress(Host(), a_buffer);
	}

	org::services::BackingPoolHooks Hooks()
	{
		auto hooks = org::services::GraphHostBackingHooks(Host());
		hooks.allocate = [allocate = std::move(hooks.allocate)](const org::services::BackingRequest& a_request) {
			auto allocation = allocate(a_request);
			if (a_request.flags.deviceAddress && allocation.buffer) {
				// A long-lived backing may move while the GPU is idle, so the pool keeps no address that would go stale.
				// Readers ask the buffer (DeviceAddressOf), and follow its moves.
				::RenderGraph::Relocation::FollowAtIdle(*allocation.buffer);
				allocation.address = 0;
			}
			return allocation;
		};
		return hooks;
	}

	org::services::BackingPoolHooks PerFrameHooks()
	{
		auto hooks = org::services::GraphHostBackingHooks(Host());
		hooks.allocate = [allocate = std::move(hooks.allocate)](const org::services::BackingRequest& a_request) {
			// Every backing of the pool is made this way, so the pool's reuse by equal flags is unchanged.
			auto request = a_request;
			request.flags.perFrame = true;
			auto allocation = allocate(request);
			// ORG never moves a per-frame backing. The pin says the same for the address it hands out (Backing::address).
			if (request.flags.deviceAddress && allocation.buffer)
				::RenderGraph::Relocation::Pin(*allocation.buffer);
			return allocation;
		};
		return hooks;
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

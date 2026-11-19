#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <OpenRenderGraph/ExternalClient.h>

#	include <cstdint>
#	include <functional>
#	include <memory>
#	include <span>
#	include <string_view>

/**
 * @brief CS's one ORG ExternalClient, for GPU work recorded outside any graph on resources the graph tracks.
 *
 * Used for rare work such as native mode's immediate-context copies into graph-owned textures. Each submission lists what it
 * uses, and its barriers come from the same ledgers the graphs use, so the next graph starts from the right state.
 * It always starts with a full memory barrier, since the native immediate context shares the queue and the ledgers do not see it.
 * Includers must include <rhi_interop_vulkan.h> first, so volk precedes any Vulkan header.
 */
namespace RenderGraph::ExternalWork
{
	using Access = org::ExternalClient::Access;

	Access CopySource(org::Resource& a_resource);

	Access CopyDestination(org::Resource& a_resource);

	/**
	 * @brief Records and submits now on the graphics queue. a_accesses lists every graph resource the work uses. Any thread.
	 * @return False when nothing was submitted, and the caller then does the work its own way.
	 */
	bool Submit(std::string_view a_label, std::span<const Access> a_accesses, const std::function<void(rhi::CommandList&)>& a_record,
		std::shared_ptr<const void> a_keepAlive = {}, std::span<const rhi::TimelinePoint> a_signals = {});

	/** @brief Waits for the client's submissions and drops it, before the host goes. */
	void Shutdown();
}

#endif

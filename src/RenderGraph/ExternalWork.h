#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <OpenRenderGraph/ExternalClient.h>

#	include <cstdint>
#	include <functional>
#	include <memory>
#	include <span>
#	include <string_view>

/**
 * @brief CS's one ORG ExternalClient: GPU work recorded outside any graph, ordered by the runtime's ledgers
 * (org-native-frame-graph.md, Phase 3).
 *
 * For rare or unforeseen work on resources the graph tracks, such as native mode's immediate-context copies and uploads into
 * graph-owned textures. Each submission lists what it uses and how. The barriers before it come from the same ledgers the graphs
 * use, so a graph that uses the resource next starts from the right state, and nothing about any graph changes.
 *
 * The client runs on the graphics queue (slot 0) with the graphs' queue slots (graphics, compute, copy), on the host's runtime.
 * - Native mode has no home states, so the submissions are open: each leaves what it used in the state it declared, and the
 *   ledgers carry that to the next user. (With closed executions, CS_ORG_CLOSED, each returns what it used to its home state.)
 * - It always starts with a full memory barrier. The native immediate context's own work shares the queue, and the ledgers do not
 *   know about it.
 *
 * Made on first use, once the render graph is active. Translation units that include this header include <rhi_interop_vulkan.h>
 * first (volk before any Vulkan header).
 */
namespace RenderGraph::ExternalWork
{
	using Access = org::ExternalClient::Access;

	/** @brief The whole of a_resource read by a copy. */
	Access CopySource(org::Resource& a_resource);

	/** @brief The whole of a_resource written by a copy. */
	Access CopyDestination(org::Resource& a_resource);

	/**
	 * @brief Records a_record and submits it on the graphics queue now, after everything submitted before it. a_accesses lists every
	 * graph resource the work uses; each must have a backing. Work on resources the graph does not track (an upload buffer) needs no
	 * entry. a_keepAlive is held until the GPU is done. a_label names the work in the log. a_signals are extra timeline points the
	 * submission signals when it is done (the native context's own timeline, so its waits cover the work). Any thread.
	 * @return False when nothing was submitted: the render graph is not active, the client could not be made, or the submission
	 * failed (logged once per label). The caller then does the work its own way.
	 */
	bool Submit(std::string_view a_label, std::span<const Access> a_accesses, const std::function<void(rhi::CommandList&)>& a_record,
		std::shared_ptr<const void> a_keepAlive = {}, std::span<const rhi::TimelinePoint> a_signals = {});

	/** @brief Waits for the client's submissions and drops it. RenderGraphRuntime::Shutdown calls it before the host goes. */
	void Shutdown();
}

#endif

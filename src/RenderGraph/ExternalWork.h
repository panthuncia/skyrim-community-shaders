#pragma once

#include <OpenRenderGraph/ExternalClient.h>
#include <rhi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

/**
 * @brief CS's one ORG ExternalClient: native mode's one way to submit GPU work outside the graphs, made with the native device.
 *
 * The native immediate context's operations, DCLF's commands outside its epochs (RenderGraphRuntime::EmitCommands) and the
 * present's copy go through it. Each submission lists the graph resources it uses, and its barriers come from the same ledgers
 * the graphs use, so the next graph starts from the right state. It always starts with a full memory barrier, since work it
 * records on untracked resources moves them itself and the ledgers do not see that.
 * Includers must include <rhi_interop_vulkan.h> first, so volk precedes any Vulkan header.
 */
namespace RenderGraph::ExternalWork
{
	using Access = org::ExternalClient::Access;

	Access CopySource(org::Resource& a_resource);

	Access CopyDestination(org::Resource& a_resource);

	/**
	 * @brief Makes the client on a_runtime, with a_device's graphics, compute and copy queues in the graphs' slot order. Native
	 * mode's device creation, once. Open submissions: native mode has no home states, so the ledgers carry what each leaves.
	 * A failure is fatal.
	 */
	void Start(std::shared_ptr<org::Runtime> a_runtime, rhi::Device a_device);

	/**
	 * @brief Records and submits now on the graphics queue, after a_waits. a_accesses lists every graph resource the work uses.
	 * Any thread. A client that cannot submit is fatal.
	 * @return False only after Shutdown, when nothing is submitted any more.
	 */
	bool Submit(std::string_view a_label, std::span<const Access> a_accesses, const std::function<void(rhi::CommandList&)>& a_record,
		std::shared_ptr<const void> a_keepAlive = {}, std::span<const rhi::TimelinePoint> a_signals = {}, std::span<const rhi::TimelinePoint> a_waits = {});

	/** @brief Waits for the client's submissions and drops it, before the host goes. */
	void Shutdown();
}


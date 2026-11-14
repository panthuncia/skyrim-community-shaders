#pragma once

#include <cstddef>
#include <cstdint>

#include <rhi.h>

/**
 * @brief Diagnostics (CS_DCLF_SITE_STATS=1): pipeline statistics of the CPU pipeline's sites, per site and frame, on BasicRHI's
 * query pools (both backends).
 *
 * Each site's draws are wrapped in one pipeline statistics query, outside any render pass: in guest mode around the site's
 * execution in DXVK's command buffer, in native mode around its late pass's recording. Each query is resolved into a readback
 * buffer and read once the site's frame signalled DCLF's CPU-frame timeline. Logged every 300 frames: input vertices and
 * primitives (what the draws submit), vertex and fragment shader invocations, comparable with the engine's D3D11 statistics
 * (CS_GPU_EVENT_STATS) on native D3D11. BasicRHI has no clipper counters, so the clipper's input and output are not logged.
 */
namespace DCLF::Direct::SiteStats
{
	bool Enabled();
	/**
	 * @brief The recording thread (DXVK's CS thread in guest mode, the render thread in native mode): begins a_site's query in a_list.
	 * a_frameValue is the CPU-frame timeline's value of the site's frame, which Collect waits for. Returns its slot, or ~0u (none).
	 */
	std::uint32_t Begin(rhi::Device a_device, rhi::CommandList& a_list, std::size_t a_site, std::uint64_t a_frameValue);
	/** @brief Ends the query Begin returned a_slot for, and resolves it into the readback buffer. */
	void End(rhi::CommandList& a_list, std::uint32_t a_slot);
	/** @brief Render thread, once a frame: reads the queries whose frames a_completed covers (the CPU-frame timeline), logs every 300 frames. */
	void Collect(std::uint64_t a_completed);
}

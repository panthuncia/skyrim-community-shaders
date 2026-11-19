#pragma once

#include <cstddef>
#include <cstdint>

#include <rhi.h>

/**
 * @brief Diagnostics (CS_DCLF_SITE_STATS=1): pipeline statistics per CPU pipeline site and frame, logged every 300 frames.
 * Comparable with CS_GPU_EVENT_STATS on native D3D11. BasicRHI has no clipper counters, so those are not logged.
 */
namespace DCLF::Direct::SiteStats
{
	bool Enabled();
	/**
	 * @brief The recording thread: begins a_site's query in a_list. Collect waits for a_frameValue on the CPU-frame timeline.
	 * Returns the slot, or ~0u for none.
	 */
	std::uint32_t Begin(rhi::Device a_device, rhi::CommandList& a_list, std::size_t a_site, std::uint64_t a_frameValue);
	/** @brief Ends the query in a_slot and resolves it into the readback buffer. */
	void End(rhi::CommandList& a_list, std::uint32_t a_slot);
	/** @brief Render thread, once a frame: reads the queries a_completed covers and logs every 300 frames. */
	void Collect(std::uint64_t a_completed);
}

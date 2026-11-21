#pragma once

#include <cstdint>

/**
 * @brief CS_GPU_LOAD's fixed GPU load as a graph compute pass at the Present anchor, used with CS_GPU_TIMERS=graph.
 * Same arithmetic and group count as the D3D11 load. Render thread only. Header free of ORG and Vulkan includes.
 */
namespace GpuLoadPass
{
	/** @brief Registers the pass. False and logged on failure, and the caller keeps the D3D11 load. */
	bool Register(std::uint32_t a_groups);

	/** @brief This frame's Present anchor runs the load. Without a call in a frame, the pass is inactive. */
	void Arm();
}

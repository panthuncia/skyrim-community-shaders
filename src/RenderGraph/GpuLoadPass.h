#pragma once

#include <cstdint>

/**
 * @brief CS_PASS_SKIP's fixed GPU load (CS_GPU_LOAD=<groups>) as a render-graph compute pass at the Present anchor, used with
 * CS_GPU_TIMERS=graph in place of the D3D11 dispatch in GpuEventTimers.cpp. Same shader arithmetic and group count.
 *
 * Render thread only. Header free of ORG/Vulkan includes.
 */
namespace GpuLoadPass
{
	/** @brief Before the render graph initializes (State, ahead of RenderGraphRuntime::Initialize): reserves the Present anchor when the load runs on the graph. */
	void ReserveAnchor();

	/**
	 * @brief Registers the pass with a_groups thread groups (x; 4 in y, as the D3D11 load). False, logged, when the Present
	 * anchor has no epoch or the program does not load: the caller keeps the D3D11 load.
	 */
	bool Register(std::uint32_t a_groups);

	/** @brief This frame's Present anchor runs the load. Without a call in a frame, the pass is inactive. */
	void Arm();
}

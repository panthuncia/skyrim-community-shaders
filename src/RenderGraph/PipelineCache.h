#pragma once

// The render graph device's on-disk pipeline cache, so a pipeline built in one run is not compiled again in the next.
// DCLF's Lighting pipelines took about 300 ms each to build on a GTX 1070 Ti without it.

#include <memory>
#include <string>

#include <rhi.h>

namespace RenderGraph::PipelineCache
{
	/**
	 * @brief The cache file for a backend, inside the shader store's folder (which a start does not clear). Empty when
	 * CS_ORG_PIPELINE_CACHE=0 turns the cache off.
	 */
	std::string PathFor(rhi::Backend a_backend);

	/**
	 * @brief Render thread, once a frame. Saves the cache on a thread of its own every few seconds while pipelines are added,
	 * since the game is often ended without the device being destroyed, which would save it too. Logs the cache's state.
	 */
	void Tick(const std::shared_ptr<rhi::DevicePtr>& a_device);

	/**
	 * @brief Any thread. The next Tick saves the cache without waiting for pipelines to stop coming, if any were added since the
	 * last save. A save in progress delays it to the Tick after.
	 */
	void RequestSave();

	/** @brief Before the device goes: waits up to two seconds for a save in progress, which holds the device. */
	void WaitForSave();
}

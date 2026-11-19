#pragma once

/**
* @brief Deferred::StartDeferred's GBuffer clears as one render pass, last at the Prepass anchor. On whenever the render graph runs.
* CS_ORG_DEFERRED_CLEARS=0 keeps the engine's clears.
*
* The pass loads each target as a clear and StartDeferred then binds them with SRTM_NO_CLEAR. A new clear colour re-records the pass with no build.
* When the pass is not in the anchor's built graph, StartDeferred keeps the engine's clears. Render thread. Free of ORG/Vulkan includes.
*/
namespace ORGDeferredClears
{
	bool Requested();

	/** @brief Call before the render graph initializes. */
	void Reserve();

	/** @brief At Deferred::SetupResources, once the GBuffer targets exist. */
	void Setup();

	/** @brief Before the Prepass anchor runs. True means bind with SRTM_NO_CLEAR, false keeps SRTM_CLEAR. */
	bool TakeClears();
}

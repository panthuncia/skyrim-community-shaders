#pragma once

#include "RenderGraph/Port.h"

#include <memory>
#include <string_view>

/**
* @brief Deferred::StartDeferred's GBuffer clears as one render pass, last at the Prepass anchor, in graph mode.
*
* The pass loads each target as a clear and StartDeferred then binds them with SRTM_NO_CLEAR. A new clear colour re-records the pass with no build.
* When the pass is not in the anchor's built graph, StartDeferred keeps the engine's clears. Render thread. Free of ORG/Vulkan includes.
*/
class ORGDeferredClears : public RenderGraph::Port<ORGDeferredClears>
{
public:
	static constexpr std::string_view kName = "Deferred clears";

	/** @brief At Deferred::SetupResources, once the GBuffer targets exist. Outside graph mode it does nothing; in graph mode a failure is fatal. */
	void Setup();

	/** @brief Before the Prepass anchor runs. True means bind with SRTM_NO_CLEAR, false keeps SRTM_CLEAR. */
	bool TakeClears() const;
	~ORGDeferredClears();

private:
	friend class RenderGraph::Port<ORGDeferredClears>;
	ORGDeferredClears();

	struct Impl;
	std::unique_ptr<Impl> impl;
};

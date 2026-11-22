#pragma once

#include "RenderGraph/Port.h"

#include <memory>
#include <string_view>

/**
* @brief Volumetric Lighting's GPU work on the render graph, in graph mode.
*
* The engine's CPU side runs as before. Inside the VL render every DispatchCSShader is withheld, and the apply and blur are
* detoured whole. The blur's call runs the VolumetricLighting anchor, which holds all the passes, each gated except the depth copy.
* The density volume is a declared transient on the feature's size settings and the Interior variable (frame revision plan 6.12),
* and the client follows it and the inputs the VL render finds: each registration makes its own volume, aliased in its builds'
* pool. A change switches in with its revision; until then the old one draws. The feature gives the engine the adopted revision's
* quality, so the engine's volume size is always the adopted registration's (a mismatch is fatal).
* The engine subsystems install it, not the feature. Native mode installs it with the feature off too, since it has no D3D11 path,
* and then draws the engine's own volumetric lighting. Its volume follows the engine's size (Vars::VolumetricLightingSize), and
* frames whose size the adopted revision does not have yet skip the work. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGVolumetricLighting : public RenderGraph::Port<ORGVolumetricLighting>
{
public:
	static constexpr std::string_view kName = "Volumetric Lighting";

	static void Install();

	/**
	* @brief After the features' SetupResources. Declares the volume and the client. The client registers in the first revision
	* after a VL render found its inputs (the sky's noise gradient loads late). Outside graph mode it does nothing; in graph mode a
	* failure is fatal. Once set up (IsActive), the detours take the engine's work.
	*/
	void Setup();

	/** @brief True when the dispatch is the VL render's on the ORG path, and the caller skips it. a_computeShader is for the log. */
	bool TakeEngineDispatch(std::string_view a_computeShader);
	~ORGVolumetricLighting();

private:
	friend class RenderGraph::Port<ORGVolumetricLighting>;
	friend struct ORGVolumetricLightingDetours;
	struct Impl;

	ORGVolumetricLighting();

	std::unique_ptr<Impl> impl;
};

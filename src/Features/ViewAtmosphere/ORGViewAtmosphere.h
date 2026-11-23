#pragma once

#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Port.h"

#include <d3d11.h>
#include <memory>
#include <string_view>

/**
* @brief Atmosphere and Fog on the render graph, in guest and native mode. The plain D3D11 mode keeps ViewAtmosphere's own path.
*
* Two clients, both declared at Setup and following the frame revisions (RenderGraph/Frame/Declare.h):
* - "cs.view-atmosphere" at the Prepass anchor owns the medium the shaders read through Common/ViewAtmosphere.hlsli: the
*   luminance and transmittance atlases and the parameters buffer. They are published once, and exported for DCLF's frame
*   textures at t104, t107 and t108. The atlas is declared on the size settings and the display size. While the shared volume
*   is on (SharedVolumeAt), the client also runs its inject and integrate passes (ORGViewAtmosphereVolume) and the atlas has its
*   full size. Otherwise the atlas is one texel, the parameters say "no medium", and Exponential Height Fog's own graph volumes
*   are the fog.
* - "cs.view-atmosphere.composite" at EngineDraws, after the water: the post-water composite of the medium into kMAIN.
* - "cs.view-atmosphere.composite" at EngineDraws, after the water: the post-water composite of the medium, Physical Sky's
*   clouds and lightning into kMAIN. On while the fog is on or Physical Sky renders, as on D3D11. Its steps are in the
*   deferred+water and sun stages, beside the water's. It switches in with the revision that holds it, and PostWaterComposite
*   with it.
* Setup failures are fatal. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGViewAtmosphere : public RenderGraph::Port<ORGViewAtmosphere>
{
public:
	static constexpr std::string_view kName = "Atmosphere and Fog";

	static constexpr std::string_view kLuminanceId = "ViewAtmosphere::Luminance";
	static constexpr std::string_view kTransmittanceId = "ViewAtmosphere::Transmittance";
	static constexpr std::string_view kParametersId = "ViewAtmosphere::Parameters";

	/** @brief At SetupResources: samplers, gate, latch, the medium and both clients' declarations. Programs build on the program thread. */
	void Setup();

	/**
	* @brief At Deferred::PrepassPasses, before the features' Prepass: stages the frame's parameters, and notes what the composite
	* can read (whether Physical Sky would render, its program built, its inputs published, the clouds' targets written). A change is
	* taken when the next revision starts.
	*/
	void Prepass();

	/**
	* @brief Whether the adopted revision holds the post-water composite, so it composes this frame's medium.
	* SharedData::PostWaterComposite follows it. Stable within a frame.
	*/
	bool CompositeActive() const;

	/** @brief The post-water hook (Deferred::Hooks::PostWater_SetCamera). Runs the composite once a main-view frame. */
	void CompositeAfterWater();

	/**
	* @brief Whether the adopted revision runs the shared volume, so it replaces Exponential Height Fog's volumes and Physical Sky's
	* AP shadow this frame. ViewAtmosphere::IsPrepared answers it in the graph modes. Stable within a frame.
	*/
	bool SharedVolumeActive() const;

	/** @brief What SharedVolumeAt reads, for a declaration that follows the shared volume. Empty before Setup. */
	RenderGraph::Frame::Dependencies SharedVolumeDependencies() const;

	/**
	* @brief Whether the shared volume runs for a revision's values: the toggle, Physical Sky rendering, and the post-water
	* composite on, as ViewAtmosphere::Prepare asks on D3D11. The fog's volumes are one texel while it does (their existence rule),
	* and their ids stay published. False before Setup.
	*/
	bool SharedVolumeAt(const RenderGraph::Frame::Inputs& a_in) const;

	/** @brief Whether the medium is made and published, so DCLF's frame textures read its ids. False before the first Prepass. */
	bool MediumPublished() const;

	/** @brief The composite's program builds again. Once it is built, the next revision registers the composite with it. */
	void ClearShaderCache();
	~ORGViewAtmosphere();

private:
	friend class RenderGraph::Port<ORGViewAtmosphere>;
	ORGViewAtmosphere();

	struct Impl;
	std::unique_ptr<Impl> impl;
};

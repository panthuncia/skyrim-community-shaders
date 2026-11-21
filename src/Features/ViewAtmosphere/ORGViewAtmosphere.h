#pragma once

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
*   textures at t104, t107 and t108. The atlas is declared on the size settings and the display size. The shared volume's inject
*   and integrate are not on the graph yet, so the atlas is one texel, the parameters say "no medium", and Exponential Height
*   Fog's own graph volumes stay the fog.
* - "cs.view-atmosphere.composite" at EngineDraws, after the water: the post-water composite of the medium into kMAIN.
*   Its steps are in the deferred+water and sun stages, beside the water's. It switches in with the revision that holds it, and
*   PostWaterComposite with it.
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
	* can read (Physical Sky off, its program built, its inputs published). A change is taken when the next revision starts.
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
	* @brief Whether the shared volume replaces Exponential Height Fog's volumes this frame. Not ported: always false.
	* Once it is, the fog's volumes go to one texel through their declaration (the existence rule), and their ids stay published.
	*/
	bool SharedVolumeActive() const { return false; }

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

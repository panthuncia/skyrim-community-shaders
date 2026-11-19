#pragma once

#include <d3d11.h>
#include <memory>
#include <string_view>

/**
* @brief Atmosphere and Fog on the render graph, in guest and native mode. The plain D3D11 mode keeps ViewAtmosphere's own path.
*
* Two clients:
* - "cs.view-atmosphere" at the Prepass anchor owns the medium the shaders read through Common/ViewAtmosphere.hlsli: the
*   luminance and transmittance atlases and the parameters buffer. They are published, and exported for DCLF's frame textures
*   at t104, t107 and t108. The shared volume's inject and integrate are not on the graph yet, so the parameters say "no medium"
*   and Exponential Height Fog's own graph volumes stay the fog.
* - "cs.view-atmosphere.composite" at EngineDraws, after the water: the post-water composite of the medium into kMAIN.
*   Its steps are in the deferred+water and sun stages, beside the water's.
* Setup failures are fatal in a graph mode. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGViewAtmosphere
{
public:
	static constexpr std::string_view kLuminanceId = "ViewAtmosphere::Luminance";
	static constexpr std::string_view kTransmittanceId = "ViewAtmosphere::Transmittance";
	static constexpr std::string_view kParametersId = "ViewAtmosphere::Parameters";

	/** @brief True in the graph modes, where ViewAtmosphere does no D3D11 work. */
	static bool Requested();

	static ORGViewAtmosphere& Get();

	/** @brief Reserves Prepass and EngineDraws. Call before the render graph initializes. */
	static void Reserve();

	/** @brief At SetupResources: samplers, gate and latch. Programs build on the program thread. */
	void Setup();

	/** @brief At Deferred::PrepassPasses, before the features' Prepass: makes and registers on first use, and stages the frame's parameters. */
	void Prepass();

	/** @brief Whether the post-water composite composes this frame's medium. SharedData::PostWaterComposite follows it. */
	bool CompositeActive() const;

	/** @brief The post-water hook (Deferred::Hooks::PostWater_SetCamera). Runs the composite once a main-view frame. */
	void CompositeAfterWater();

	/** @brief Whether the shared volume replaces Exponential Height Fog's volumes this frame. Not ported: always false. */
	bool SharedVolumeActive() const { return false; }

	/** @brief The exports DCLF's frame textures read. Null before the first Prepass. */
	ID3D11ShaderResourceView* LuminanceSRV() const;
	ID3D11ShaderResourceView* TransmittanceSRV() const;
	ID3D11ShaderResourceView* ParametersSRV() const;

	/** @brief The composite's program builds again, and its client registers again once it is built. */
	void ClearShaderCache();

	~ORGViewAtmosphere();

private:
	ORGViewAtmosphere() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};

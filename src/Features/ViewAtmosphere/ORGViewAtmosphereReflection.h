#pragma once

#include "RenderGraph/Port.h"

#include <memory>
#include <string_view>

/**
* @brief Atmosphere and Fog's reflection medium on the render graph, in guest and native mode (view-atmosphere-port.md 3.7).
*
* Client "cs.view-atmosphere.reflection" at the ReflectionsPrepass anchor (start stage), declared at Setup. Each cubemap render
* it runs what ViewAtmosphere::RenderReflection runs on D3D11, on the same conditions: the sky SH (Reflection.cs.hlsl with
* REFLECTION_SKY_SH, one group), then the 32^3 omnidirectional medium (16x32 groups). It reads Physical Sky's LUTs and cloud
* outputs, IBL, Skylighting, Terrain Shadows and Cloud Shadows by their published ids.
*
* Its four outputs are published at Setup, for the session:
* - kLuminanceId, kTransmittanceId, kParametersId: DCLF's reflection faces read them at t104, t107 and t108 in place of the main
*   medium (per-epoch views, Api::FrameTexture::facesGraphResources). The parameters are copied every run: no medium (all zero)
*   on runs the medium does not run, so the faces then do without it, as D3D11's unbound t127 does.
* - kSkySHId: the sky SH in row 0, the cloud diffuse transport in row 1 (D3D11's CloudDiffuseTransport(), t70). Physical Sky's
*   sky tail and the shared volume's light scattering read it by id. It is born zero, which reads as no transport (w = 0), and
*   cleared to zero when the medium stops running.
*
* Setup failures are fatal. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGViewAtmosphereReflection : public RenderGraph::Port<ORGViewAtmosphereReflection>
{
public:
	static constexpr std::string_view kName = "Atmosphere and Fog reflections";

	static constexpr std::string_view kLuminanceId = "ViewAtmosphere::ReflectionLuminance";
	static constexpr std::string_view kTransmittanceId = "ViewAtmosphere::ReflectionTransmittance";
	static constexpr std::string_view kParametersId = "ViewAtmosphere::ReflectionParameters";
	static constexpr std::string_view kSkySHId = "ViewAtmosphere::ReflectionSkySH";

	/** @brief At SetupResources, after ORGViewAtmosphere's. Programs, outputs and the client's declaration. Nothing without Physical Sky. */
	void Setup();

	/**
	* @brief At ViewAtmosphere::RenderReflection, after the ReflectionsPrepass anchor ran. Notes what the passes can read: a change
	* registers the client again when the next revision starts.
	*/
	void Observe();

	/** @brief Whether kSkySHId is published, so readers can bind the cloud diffuse transport by id. Stable after Setup. */
	bool TransportPublished() const;

	~ORGViewAtmosphereReflection();

private:
	friend class RenderGraph::Port<ORGViewAtmosphereReflection>;
	ORGViewAtmosphereReflection();

	struct Impl;
	std::shared_ptr<Impl> impl;
};

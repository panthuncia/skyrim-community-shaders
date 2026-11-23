// Atmosphere and Fog: its medium's atlases and parameters (t104, t107, t108) as frame textures. Callbacks read the feature live.
// The reflection faces read the reflection medium there instead (per-epoch views, view-atmosphere-port.md open question 2).

#include "Features/DrawcallLimitFix/Integrations/ViewAtmosphere/ViewAtmosphere.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Features/ViewAtmosphere.h"
#include "Features/ViewAtmosphere/ORGViewAtmosphere.h"
#include "Features/ViewAtmosphere/ORGViewAtmosphereReflection.h"

namespace DCLF::Integrations::ViewAtmosphere
{
	namespace
	{
		// The published medium's ids, so the commits read the graph resources, ordered after the Prepass anchor. It has no D3D11 views.
		template <const std::string_view& Id>
		std::string_view MediumId()
		{
			return ORGViewAtmosphere::Get().MediumPublished() ? Id : std::string_view{};
		}

		// No D3D11 view: until the medium is published the commits read the null texture.
		void* NoView() { return nullptr; }

		// The faces' epochs read the reflection medium's ids, published at setup (ORGViewAtmosphereReflection). Its parameters say
		// no medium on runs it does not run, as D3D11's unbound t127 in the faces. Without it the faces read the null texture.
		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "atmosphere and fog luminance", .textureRegister = kLuminanceRegister, .view = &NoView, .graphResources = { ORGViewAtmosphere::kLuminanceId }, .facesGraphResources = { ORGViewAtmosphereReflection::kLuminanceId }, .graphId = &MediumId<ORGViewAtmosphere::kLuminanceId> });
			a_context.RegisterFrameTexture({ .name = "atmosphere and fog transmittance", .textureRegister = kTransmittanceRegister, .view = &NoView, .graphResources = { ORGViewAtmosphere::kTransmittanceId }, .facesGraphResources = { ORGViewAtmosphereReflection::kTransmittanceId }, .graphId = &MediumId<ORGViewAtmosphere::kTransmittanceId> });
			a_context.RegisterFrameTexture({ .name = "atmosphere and fog parameters", .textureRegister = kParametersRegister, .view = &NoView, .graphResources = { ORGViewAtmosphere::kParametersId }, .facesGraphResources = { ORGViewAtmosphereReflection::kParametersId }, .graphId = &MediumId<ORGViewAtmosphere::kParametersId> });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "atmosphere and fog", .install = &Install, .status = [] { return FeatureStatus(globals::features::viewAtmosphere); } };
	}
}

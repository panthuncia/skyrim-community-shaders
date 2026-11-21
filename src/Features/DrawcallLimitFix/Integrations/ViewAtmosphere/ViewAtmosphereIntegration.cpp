// Atmosphere and Fog: its medium's atlases and parameters (t104, t107, t108) as frame textures. Callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/ViewAtmosphere/ViewAtmosphere.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Features/ViewAtmosphere.h"
#include "Features/ViewAtmosphere/ORGViewAtmosphere.h"

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

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "atmosphere and fog luminance", .textureRegister = kLuminanceRegister, .view = &NoView,
				.graphResources = { ORGViewAtmosphere::kLuminanceId }, .graphId = &MediumId<ORGViewAtmosphere::kLuminanceId> });
			a_context.RegisterFrameTexture({ .name = "atmosphere and fog transmittance", .textureRegister = kTransmittanceRegister, .view = &NoView,
				.graphResources = { ORGViewAtmosphere::kTransmittanceId }, .graphId = &MediumId<ORGViewAtmosphere::kTransmittanceId> });
			a_context.RegisterFrameTexture({ .name = "atmosphere and fog parameters", .textureRegister = kParametersRegister, .view = &NoView,
				.graphResources = { ORGViewAtmosphere::kParametersId }, .graphId = &MediumId<ORGViewAtmosphere::kParametersId> });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "atmosphere and fog", .install = &Install, .status = [] { return FeatureStatus(globals::features::viewAtmosphere); } };
	}
}

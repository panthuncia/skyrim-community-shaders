// Atmosphere and Fog: its medium's atlases and parameters (t104, t107, t108) as frame textures. Callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/ViewAtmosphere/ViewAtmosphere.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Features/ViewAtmosphere.h"
#include "Features/ViewAtmosphere/ORGViewAtmosphere.h"

namespace DCLF::Integrations::ViewAtmosphere
{
	namespace
	{
		// The exports of the published medium, so the commits read the graph resources, ordered after the Prepass anchor.
		void* ViewAtmosphereLuminanceView() { return ORGViewAtmosphere::Get().LuminanceSRV(); }

		void* ViewAtmosphereTransmittanceView() { return ORGViewAtmosphere::Get().TransmittanceSRV(); }

		void* ViewAtmosphereParametersView() { return ORGViewAtmosphere::Get().ParametersSRV(); }

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "atmosphere and fog luminance", .textureRegister = kLuminanceRegister,
				.view = &ViewAtmosphereLuminanceView, .graphResources = { ORGViewAtmosphere::kLuminanceId } });
			a_context.RegisterFrameTexture({ .name = "atmosphere and fog transmittance", .textureRegister = kTransmittanceRegister,
				.view = &ViewAtmosphereTransmittanceView, .graphResources = { ORGViewAtmosphere::kTransmittanceId } });
			a_context.RegisterFrameTexture({ .name = "atmosphere and fog parameters", .textureRegister = kParametersRegister,
				.view = &ViewAtmosphereParametersView, .graphResources = { ORGViewAtmosphere::kParametersId } });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "atmosphere and fog", .install = &Install, .status = [] { return FeatureStatus(globals::features::viewAtmosphere); } };
	}
}

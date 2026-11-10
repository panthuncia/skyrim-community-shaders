// Exponential Height Fog: its integrated light scattering volumes (t19 near, t22 far), frame textures. The render thread's callbacks
// read the feature live.

#include "Features/DrawcallLimitFix/Integrations/ExponentialHeightFog/ExponentialHeightFog.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Features/ExponentialHeightFog.h"
#include "Features/ExponentialHeightFog/ORGExponentialHeightFog.h"

namespace DCLF::Integrations::ExponentialHeightFog
{
	namespace
	{
		// The views Prepass binds at t19 and t22 under its own conditions (every path past them ends in BindIntegratedLightScattering;
		// otherwise ReleaseVolumetricResources binds null). Prepass runs after the faces' commit, so they read last frame's volumes, or the
		// previous textures while Prepass remakes them (a grid change), held by their binding. Null without the feature or the volumes:
		// the captured views.
		bool Binds()
		{
			const auto& fog = globals::features::exponentialHeightFog;
			const auto& settings = fog.settings;
			return fog.loaded && settings.enabled && settings.volumetricFogEnabled && settings.volumetricFogExtinctionScale > 0.0f;
		}

		void* LightScatteringView()
		{
			return Binds() ? globals::features::exponentialHeightFog.IntegratedLightScatteringView() : nullptr;
		}

		void* LightScatteringFarView()
		{
			return Binds() ? globals::features::exponentialHeightFog.IntegratedLightScatteringFarView() : nullptr;
		}

		// On the ORG path (CS_ORG_EXPONENTIAL_HEIGHT_FOG) the views are the graph volumes' exports while the graph ran the last Prepass, so
		// the commits read the published volumes, ordered after the integration at the Prepass anchor.
		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "exponential height fog light scattering", .textureRegister = kLightScatteringRegister,
				.view = &LightScatteringView, .capturedWhenNull = true, .graphResources = { ORGExponentialHeightFog::kNearIntegratedId } });
			a_context.RegisterFrameTexture({ .name = "exponential height fog far light scattering", .textureRegister = kLightScatteringFarRegister,
				.view = &LightScatteringFarView, .capturedWhenNull = true, .graphResources = { ORGExponentialHeightFog::kFarIntegratedId } });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "exponential height fog", .install = &Install, .status = [] { return FeatureStatus(globals::features::exponentialHeightFog); } };
	}
}

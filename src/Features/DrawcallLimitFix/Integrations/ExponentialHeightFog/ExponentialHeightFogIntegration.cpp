// Exponential Height Fog: its light scattering volumes (t19 near, t22 far) as frame textures. Callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/ExponentialHeightFog/ExponentialHeightFog.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Features/ExponentialHeightFog.h"
#include "Features/ExponentialHeightFog/ORGExponentialHeightFog.h"

namespace DCLF::Integrations::ExponentialHeightFog
{
	namespace
	{
		// The views Prepass binds at t19 and t22. Prepass runs after the faces' commit, so the faces read last frame's volumes,
		// or the previous textures while Prepass remakes them on a grid change. Null without the feature or volumes.
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

		// On the ORG path the views are the graph volumes' exports, so commits read them ordered after the integration at the Prepass anchor.
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

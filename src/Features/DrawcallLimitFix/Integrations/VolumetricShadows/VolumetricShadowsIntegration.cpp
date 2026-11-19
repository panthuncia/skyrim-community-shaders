// Volumetric Shadows: its shared shadow map (t18), a frame texture. The render thread's callback reads the feature live.

#include "Features/DrawcallLimitFix/Integrations/VolumetricShadows/VolumetricShadows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Features/VolumetricShadows.h"
#include "Features/VolumetricShadows/ORGVolumetricShadows.h"

namespace DCLF::Integrations::VolumetricShadows
{
	namespace
	{
		static_assert(::VolumetricShadows::kSharedShadowMapShaderSlot == kSharedShadowMapRegister);

		// The view CopyShadowLightData last bound at t18: a persistent downsampled copy made at its first copy.
		// At the faces' commit it still holds last frame's shadows, since it precedes this frame's shadow mask. Null otherwise.
		void* SharedShadowMapView()
		{
			return globals::features::volumetricShadows.loaded ? ::VolumetricShadows::boundSharedShadowMap : nullptr;
		}

		// On the ORG path the bound view is the graph texture's export, so commits read it ordered after the blurs at the EarlyPrepass anchor.
		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "volumetric shadows shadow map", .textureRegister = kSharedShadowMapRegister,
				.view = &SharedShadowMapView, .capturedWhenNull = true, .graphResources = { ORGVolumetricShadows::kShadowCopyId } });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "volumetric shadows", .install = &Install, .status = [] { return FeatureStatus(globals::features::volumetricShadows); } };
	}
}

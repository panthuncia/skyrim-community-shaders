// Screen Space Shadows: its shadow texture (t45), a frame texture on its graph path.

#include "Features/DrawcallLimitFix/Integrations/ScreenSpaceShadows/ScreenSpaceShadows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Features/ScreenSpaceShadows.h"
#include "Features/ScreenSpaceShadows/ORGScreenSpaceShadows.h"

namespace DCLF::Integrations::ScreenSpaceShadows
{
	namespace
	{
		// t45 has no view of its own: the colour commit reads the captured view, the faces null. On the ORG path
		// (CS_ORG_SCREEN_SPACE_SHADOWS) the Prepass binds the graph texture's export, so the commit reads the published texture,
		// ordered after the raymarch at the Prepass anchor.
		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "screen space shadows", .textureRegister = kShadowTextureRegister,
				.graphResources = { ORGScreenSpaceShadows::kShadowTextureId } });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "screen space shadows", .install = &Install, .status = [] { return FeatureStatus(globals::features::screenSpaceShadows); } };
	}
}

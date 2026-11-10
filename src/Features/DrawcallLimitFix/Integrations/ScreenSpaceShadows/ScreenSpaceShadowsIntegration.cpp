// Screen Space Shadows: its shadow texture (t45), a frame texture on its graph path. The render thread's callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/ScreenSpaceShadows/ScreenSpaceShadows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "RenderGraph/PublishedResources.h"

#include "Features/ScreenSpaceShadows.h"
#include "Features/ScreenSpaceShadows/ORGScreenSpaceShadows.h"

namespace DCLF::Integrations::ScreenSpaceShadows
{
	namespace
	{
		// The view the Prepass binds at t45 is the colour commit's captured one: the integration names none. The views with frame
		// constants of their own (the reflection's faces) read null there, as before the registration.
		void* ShadowTextureView()
		{
			return nullptr;
		}

		// On the ORG path (CS_ORG_SCREEN_SPACE_SHADOWS) the Prepass binds the graph texture's D3D11 export: the colour passes read the
		// published texture instead, ordered after the raymarch at the Prepass anchor. ShadowSRV is null exactly when the D3D11 path
		// bound its own texture.
		bool ShadowTextureGraphIndex(std::uint32_t& a_index)
		{
			return globals::features::screenSpaceShadows.loaded && ORGScreenSpaceShadows::Get().ShadowSRV() &&
			       PublishedResources::ShaderResourceIndex(ORGScreenSpaceShadows::kShadowTextureId, a_index);
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "screen space shadows", .textureRegister = kShadowTextureRegister, .view = &ShadowTextureView,
				.capturedWhenNull = true, .graphResource = ORGScreenSpaceShadows::kShadowTextureId, .graph = &ShadowTextureGraphIndex });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "screen space shadows", .install = &Install, .status = [] { return FeatureStatus(globals::features::screenSpaceShadows); } };
	}
}

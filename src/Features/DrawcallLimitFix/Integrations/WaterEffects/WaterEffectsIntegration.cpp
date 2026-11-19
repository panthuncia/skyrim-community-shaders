// Water Effects: its caustics texture (t65), a frame texture. The render thread's callback reads the feature live.

#include "Features/DrawcallLimitFix/Integrations/WaterEffects/WaterEffects.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Features/WaterEffects.h"

namespace DCLF::Integrations::WaterEffects
{
	namespace
	{
		// The view Prepass binds at t65 every frame: a static caustics texture loaded at setup. Null without the feature or the texture.
		void* CausticsView()
		{
			const auto& waterEffects = globals::features::waterEffects;
			return waterEffects.loaded ? waterEffects.causticsView.get() : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "water effects caustics", .textureRegister = kCausticsRegister, .view = &CausticsView,
				.capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "water effects", .install = &Install, .status = [] { return FeatureStatus(globals::features::waterEffects); } };
	}
}

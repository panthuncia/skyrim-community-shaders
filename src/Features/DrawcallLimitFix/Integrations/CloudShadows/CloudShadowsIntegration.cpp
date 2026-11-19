// Cloud Shadows: its occlusion cubemap (t25) and self-shadowing input (t26) as frame textures. Callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/CloudShadows/CloudShadows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "State.h"

#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"

namespace DCLF::Integrations::CloudShadows
{
	namespace
	{
		// The view ReflectionsPrepass binds at t25, a persistent copy of the last cloud layer.
		// Only on a frame whose water reflection ran that prepass with the full sky and a climate. Otherwise the captured view.
		// On the ORG path the view is the published cubemap's export, so commits read it ordered after its copy.
		bool Bound()
		{
			const auto& cloudShadows = globals::features::cloudShadows;
			if (!cloudShadows.loaded || !globals::state->activeReflections || !cloudShadows.OcclusionSRV())
				return false;
			const auto* sky = globals::game::sky;
			return sky && sky->mode.get() == RE::Sky::Mode::kFull && sky->currentClimate;
		}

		void* CloudOcclusionView()
		{
			return Bound() ? globals::features::cloudShadows.OcclusionSRV() : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "cloud shadows occlusion", .textureRegister = kCloudOcclusionRegister,
				.view = &CloudOcclusionView, .capturedWhenNull = true, .graphResources = { ORGCloudShadows::kOcclusionId } });
			// t26 has no view of its own. The colour commit reads the captured view and the faces null.
			// After the reflections capture that view is the occlusion cubemap, which on the ORG path is the published export.
			a_context.RegisterFrameTexture({ .name = "cloud shadows self shadow", .textureRegister = kSelfShadowRegister,
				.graphResources = { ORGCloudShadows::kOcclusionId } });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "cloud shadows", .install = &Install, .status = [] { return FeatureStatus(globals::features::cloudShadows); } };
	}
}

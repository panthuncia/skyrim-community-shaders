// Cloud Shadows: its cloud occlusion cubemap (t25) and its self-shadowing input (t26), frame textures. The render thread's callbacks
// read the feature live.

#include "Features/DrawcallLimitFix/Integrations/CloudShadows/CloudShadows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "State.h"

#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"

namespace DCLF::Integrations::CloudShadows
{
	namespace
	{
		// The view ReflectionsPrepass binds at t25 (the only PS binding there): its cloud occlusion copy, a persistent texture, copied in the
		// same prepass (this frame's at the faces' commit). Only on a frame whose water reflection ran that prepass (State::activeReflections,
		// set by Deferred::ReflectionsPrepasses until Present) with the full sky and a climate, its conditions; otherwise the binding stands
		// from an earlier frame, or was never made: the captured view.
		// On the ORG path (CS_ORG_CLOUD_SHADOWS) the view is the published cubemap's export, so the commits read the published cubemap,
		// ordered after its copy.
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
			// t26 has no view of its own: the colour commit reads the captured view, the faces null. After the reflections capture the
			// captured view is the occlusion cubemap, which on the ORG path is the published cubemap's export.
			a_context.RegisterFrameTexture({ .name = "cloud shadows self shadow", .textureRegister = kSelfShadowRegister,
				.graphResources = { ORGCloudShadows::kOcclusionId } });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "cloud shadows", .install = &Install, .status = [] { return FeatureStatus(globals::features::cloudShadows); } };
	}
}

// Cloud Shadows: its cloud occlusion cubemap (t25) and its self-shadowing input (t26), frame textures. The render thread's callbacks
// read the feature live.

#include "Features/DrawcallLimitFix/Integrations/CloudShadows/CloudShadows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "RenderGraph/PublishedResources.h"
#include "State.h"

#include "Features/CloudShadows.h"

namespace DCLF::Integrations::CloudShadows
{
	namespace
	{
		// The view ReflectionsPrepass binds at t25 (the only PS binding there): its cloud occlusion copy, a persistent texture, copied in the
		// same prepass (this frame's at the faces' commit). Only on a frame whose water reflection ran that prepass (State::activeReflections,
		// set by Deferred::ReflectionsPrepasses until Present) with the full sky and a climate, its conditions; otherwise the binding stands
		// from an earlier frame, or was never made: the captured view.
		// On the ORG path (CS_ORG_CLOUD_SHADOWS) the colour passes read the published cubemap instead, ordered after its copy.
		constexpr std::string_view kOcclusionId = "CloudShadows::Occlusion";

		bool Bound()
		{
			const auto& cloudShadows = globals::features::cloudShadows;
			if (!cloudShadows.loaded || !globals::state->activeReflections || !cloudShadows.texCubemapCloudOccCopy)
				return false;
			const auto* sky = globals::game::sky;
			return sky && sky->mode.get() == RE::Sky::Mode::kFull && sky->currentClimate;
		}

		void* CloudOcclusionView()
		{
			return Bound() ? globals::features::cloudShadows.OcclusionSRV() : nullptr;
		}

		bool CloudOcclusionGraphIndex(std::uint32_t& a_index)
		{
			return Bound() && PublishedResources::ShaderResourceIndex(kOcclusionId, a_index);
		}

		// The view at t26 is the colour commit's captured one: the integration names none. The views with frame constants of their own
		// (the reflection's faces) read null there, as before the registration.
		void* SelfShadowView()
		{
			return nullptr;
		}

		// The captured view decides alone. After the reflections capture it is the occlusion cubemap, which on the ORG path is the
		// published cubemap's export, and the commit then reads the published cubemap (Extensions::FrameGraphTextureIndex). A layer's view
		// or the D3D11 copy stays the captured view. Cloud Shadows' current bind is not asked: the main sky rebinds t26 before the commit
		// resolves its textures.
		bool SelfShadowGraphIndex(std::uint32_t&)
		{
			return false;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "cloud shadows occlusion", .textureRegister = kCloudOcclusionRegister,
				.view = &CloudOcclusionView, .capturedWhenNull = true, .graphResource = kOcclusionId, .graph = &CloudOcclusionGraphIndex });
			a_context.RegisterFrameTexture({ .name = "cloud shadows self shadow", .textureRegister = kSelfShadowRegister, .view = &SelfShadowView,
				.capturedWhenNull = true, .graphResource = kOcclusionId, .graph = &SelfShadowGraphIndex });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "cloud shadows", .install = &Install, .status = [] { return FeatureStatus(globals::features::cloudShadows); } };
	}
}

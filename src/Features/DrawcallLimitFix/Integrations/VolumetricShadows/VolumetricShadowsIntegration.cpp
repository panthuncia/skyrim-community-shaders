// Volumetric Shadows: its shared shadow map (t18), a frame texture. The render thread's callback reads the feature live.

#include "Features/DrawcallLimitFix/Integrations/VolumetricShadows/VolumetricShadows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "RenderGraph/PublishedResources.h"

#include "Features/VolumetricShadows.h"
#include "Features/VolumetricShadows/ORGVolumetricShadows.h"

namespace DCLF::Integrations::VolumetricShadows
{
	namespace
	{
		static_assert(::VolumetricShadows::kSharedShadowMapShaderSlot == kSharedShadowMapRegister);

		// The view CopyShadowLightData last bound at t18 (SetSharedShadowMapSRV, the only PS binding there): its downsampled copy with
		// directional shadows, a persistent texture made at its first copy (last frame's shadows at the faces' commit, which precedes this
		// frame's shadow mask), else null. Null: the captured view, also null.
		void* SharedShadowMapView()
		{
			return globals::features::volumetricShadows.loaded ? ::VolumetricShadows::boundSharedShadowMap : nullptr;
		}

		// On the ORG path (CS_ORG_VOLUMETRIC_SHADOWS) the bound view is the graph texture's export: the colour passes read the published
		// texture instead, ordered after the blurs at the EarlyPrepass anchor.
		constexpr std::string_view kSharedShadowMapId = "VolumetricShadows::ShadowCopy";

		bool SharedShadowMapGraphIndex(std::uint32_t& a_index)
		{
			auto* const view = SharedShadowMapView();
			return view && view == ORGVolumetricShadows::Get().SharedShadowMapSRV() && PublishedResources::ShaderResourceIndex(kSharedShadowMapId, a_index);
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "volumetric shadows shadow map", .textureRegister = kSharedShadowMapRegister,
				.view = &SharedShadowMapView, .capturedWhenNull = true, .graphResource = kSharedShadowMapId, .graph = &SharedShadowMapGraphIndex });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "volumetric shadows", .install = &Install, .status = [] { return FeatureStatus(globals::features::volumetricShadows); } };
	}
}

// Skylighting: its probe array (t50) and shadow visibility (t53), frame textures. The render thread's callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/Skylighting/Skylighting.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "RenderGraph/PublishedResources.h"
#include "State.h"

#include "Features/Skylighting.h"
#include "Features/Skylighting/ORGSkylighting.h"

namespace DCLF::Integrations::Skylighting
{
	namespace
	{
		// Whether this frame's Prepass binds t50 and t53 (the only PS bindings there): its conditions, outside the map menu under the full
		// sky. Otherwise the bindings stand from an earlier frame, or were never made: the captured views.
		bool Binds()
		{
			if (!globals::features::skylighting.loaded || globals::state->isMapMenuOpen)
				return false;
			const auto* sky = globals::game::sky;
			return sky && sky->mode.get() == RE::Sky::Mode::kFull;
		}

		// Persistent textures, which Prepass updates before it binds them (last frame's at the faces' commit): the D3D11 ones, or the
		// render graph's exports while its probe update runs (CS_ORG_SKYLIGHTING=1).
		void* ProbeArrayView()
		{
			return Binds() ? globals::features::skylighting.ProbeArraySRV() : nullptr;
		}

		void* ShadowVisibilityView()
		{
			return Binds() ? globals::features::skylighting.ShadowVisibilitySRV() : nullptr;
		}

		// While the probe update runs on the graph the views are its exports: the colour passes read the published volumes instead,
		// ordered after the update at the Prepass anchor.
		constexpr std::string_view kProbeArrayId = "Skylighting::ProbeArray";
		constexpr std::string_view kShadowVisibilityId = "Skylighting::ShadowVisibility";

		template <const std::string_view* Id>
		bool VolumeGraphIndex(std::uint32_t& a_index)
		{
			return Binds() && ORGSkylighting::Get().IsActive() && PublishedResources::ShaderResourceIndex(*Id, a_index);
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "skylighting probes", .textureRegister = kProbeArrayRegister, .view = &ProbeArrayView,
				.capturedWhenNull = true, .graphResource = kProbeArrayId, .graph = &VolumeGraphIndex<&kProbeArrayId> });
			a_context.RegisterFrameTexture({ .name = "skylighting shadow visibility", .textureRegister = kShadowVisibilityRegister,
				.view = &ShadowVisibilityView, .capturedWhenNull = true, .graphResource = kShadowVisibilityId,
				.graph = &VolumeGraphIndex<&kShadowVisibilityId> });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "skylighting", .install = &Install, .status = [] { return FeatureStatus(globals::features::skylighting); } };
	}
}

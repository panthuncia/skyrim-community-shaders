// Skylighting: its probe array (t50), roof coverage (t51, t52) and shadow visibility (t53), frame textures. The render thread's callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/Skylighting/Skylighting.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"
#include "Snapshot/EngineReads.h"

#include "State.h"

#include "Features/Skylighting.h"
#include "Features/Skylighting/ORGSkylighting.h"

namespace DCLF::Integrations::Skylighting
{
	namespace
	{
		// Whether this frame's Prepass binds t50 to t53: outside the map menu under the full sky. Otherwise the captured views.
		bool Binds()
		{
			if (!globals::features::skylighting.loaded || globals::state->isMapMenuOpen)
				return false;
			const auto* sky = CS_ENGINE_READ(Sky, RecordTime, globals::game::sky);
			return sky && sky->mode.get() == RE::Sky::Mode::kFull;
		}

		// Persistent textures that Prepass updates before binding: D3D11 ones, or the graph's exports in graph mode.
		void* ProbeArrayView()
		{
			return Binds() ? globals::features::skylighting.ProbeArraySRV() : nullptr;
		}

		void* ShadowVisibilityView()
		{
			return Binds() ? globals::features::skylighting.ShadowVisibilitySRV() : nullptr;
		}

		void* CoverageView()
		{
			return Binds() ? globals::features::skylighting.ProbeCoverageSRV() : nullptr;
		}

		void* ZenithView()
		{
			return Binds() ? globals::features::skylighting.ZenithSRV() : nullptr;
		}

		// While the probe update runs on the graph, commits read the published volumes ordered after the update at the Prepass anchor.
		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "skylighting probes", .textureRegister = kProbeArrayRegister, .view = &ProbeArrayView,
				.capturedWhenNull = true, .graphResources = { ORGSkylighting::kProbeArrayId } });
			a_context.RegisterFrameTexture({ .name = "skylighting shadow visibility", .textureRegister = kShadowVisibilityRegister,
				.view = &ShadowVisibilityView, .capturedWhenNull = true, .graphResources = { ORGSkylighting::kShadowVisibilityId } });
			a_context.RegisterFrameTexture({ .name = "skylighting coverage", .textureRegister = kCoverageRegister, .view = &CoverageView,
				.capturedWhenNull = true, .graphResources = { ORGSkylighting::kCoverageId } });
			a_context.RegisterFrameTexture({ .name = "skylighting zenith", .textureRegister = kZenithRegister, .view = &ZenithView,
				.capturedWhenNull = true, .graphResources = { ORGSkylighting::kZenithId } });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "skylighting", .install = &Install, .status = [] { return FeatureStatus(globals::features::skylighting); } };
	}
}

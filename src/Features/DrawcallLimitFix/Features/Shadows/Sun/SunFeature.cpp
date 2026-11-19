// Shadows/Sun: the Sun source, its accumulation and camera hooks, their report lines and parity.

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Sun.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Diagnostics/SunCasters.h"

#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Shadows::Sun
{
	namespace
	{
		/**
		 * @brief Installs the Sun source's hooks before the Spot's light selection: the accumulation's, which the point lights' culls
		 * require, then the camera hook.
		 */
		void SourceHooks(Api::ShadowSourceHooks& a_hooks)
		{
			if (a_hooks.source != Api::ShadowSource::Sun)
				return;
			auto& accumulation = SunAccumulation::Get();
			accumulation.Install();
			if (accumulation.Installed())
				a_hooks.CullsInstalled();
			SunViews::Get().Install();
		}

		/** @brief Ends the frame's sun state after the full-frustum call site. */
		bool FullFrustumWindowEnd()
		{
			auto& accumulation = SunAccumulation::Get();
			accumulation.EndFullFrustumWindow();
			return accumulation.ExclusionLive();
		}

		void Install(Api::InstallContext& a_context)
		{
			// No switch turns the skip or the exclusion off. The engine never culls the whole scene for a sun DCLF draws, only the engine-only subset.
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Sun, .camera = &SunCamera, .fullFrustumWindowEnd = &FullFrustumWindowEnd });
			a_context.RegisterSunVolume({ .name = "sun views", .cascades = &SampleSunCascades, .entryPlanes = &SampleSunEntryPlanes });
			a_context.RegisterReportLines({ .name = "sun accumulation", .point = Api::ReportPoint::RenderThread, .lines = &SunAccumulationLines });
			// CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY: the sun views, cascades and entry planes against the engine's.
			if (Diagnostics::Observe().claimsOrScene) {
				SubscribeSunViewParity();
				SubscribeSunEntryPlanesParity();
			}
			// CS_DCLF_PERSISTENT_PARITY: watch for an unclaimed caster under an excluded entry on frames that skip nothing.
			if (Diagnostics::Observe().scene)
				SunCasters::Install();
		}
	}

	Api::Feature Feature()
	{
		return { .name = "sun shadows", .install = &Install, .sourceHooks = &SourceHooks, .preflight = &SunAccumulation::Preflight };
	}
}

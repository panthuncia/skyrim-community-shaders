// Shadows/Local: the Spot, Point and Focus sources, the kept lights' views, the point lights' culls, the focus camera hook, the
// entry exclusions, and their report lines and parity.

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/SunExclusion.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/ExclusionBuild.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/FocusViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LightViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Local.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Diagnostics/LightCulls.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Diagnostics/StrictLightParity.h"

#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Shadows::Local
{
	namespace
	{
		/** @brief Computes a kept spot or point light's views. */
		void LightKept(const Api::ShadowLightKept& a_light) { LightViews::Get().Update(a_light.light); }

		/**
		 * @brief Installs the point lights' culls and the Focus camera hook. Must follow the Sun's registration hooks and precede the
		 * primary cull reading NiNode's OnVisible slot, which the category filter takes.
		 */
		void SourceHooks(Api::ShadowSourceHooks& a_hooks)
		{
			if (a_hooks.source == Api::ShadowSource::Point) {
				LocalLightCull::Install();
				if (LocalLightCull::Installed())
					a_hooks.CullsInstalled();
			}
			if (a_hooks.source == Api::ShadowSource::Focus)
				FocusViews::Get().Install();
		}

		/** @brief At frame start, the point lights' culls select the exclusion the shadow epoch published. */
		void PointFrameStart(std::uint32_t a_frame) { LocalLightCull::SelectFrame(a_frame, ParabolicExclusionSlot::Get().Pending()); }

		/** @brief Puts the shadow epoch's exclusion of a_source into its slot. */
		void PublishExclusion(Api::ShadowSource a_source, std::shared_ptr<void> a_exclusion)
		{
			auto exclusion = std::static_pointer_cast<SunExclusion>(std::move(a_exclusion));
			if (a_source == Api::ShadowSource::Sun)
				SunExclusionSlot::Get().Publish(std::move(exclusion));
			else if (a_source == Api::ShadowSource::Point)
				ParabolicExclusionSlot::Get().Publish(std::move(exclusion));
		}

		/** @brief At frame start, takes the builder's translation of the sun's pending exclusion. */
		void ExclusionFrameStart() { SunExclusionSlot::Get().BeginFrame(Scene::FrameView::Get().GetSunCandidates()); }

		/** @brief The sun's pending exclusion for the primary cull's scene-list filter. Empty when none is pending. */
		void ListExclusion(Api::ShadowListExclusion& a_view)
		{
			auto pending = SunExclusionSlot::Get().Pending();
			if (!pending)
				return;
			a_view.candidates = pending->candidates.get();
			a_view.excluded = pending->excluded;
			a_view.version = pending->version;
			a_view.held = std::move(pending);
		}

		void Install(Api::InstallContext& a_context)
		{
			const auto strictParity = a_context.RegisterSwitch({ .name = "CS_DCLF_STRICT_LIGHT_PARITY", .kind = Api::SwitchKind::Parity,
				.description = "1: the portal-strict point lights' native culls against DCLF's model of them, on the parity frames (AE)" });
			StrictLightParity::Configure(SwitchEnabled(strictParity));
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Spot, .camera = &LightCamera, .lightKept = &LightKept });
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Point, .camera = &LightCamera, .lightKept = &LightKept, .frameStart = &PointFrameStart,
				.frameEnd = &LocalLightCull::EndFrame });
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Focus, .camera = &FocusCamera });
			a_context.RegisterShadowExclusion({ .name = "entry exclusions", .build = &BuildExclusion, .publish = &PublishExclusion,
				.frameStart = &ExclusionFrameStart, .listExclusion = &ListExclusion });
			a_context.RegisterStandInObserver({ .structure = &LocalLightCull::NoteStructure });
			a_context.RegisterReportLines({ .name = "point light culls", .point = Api::ReportPoint::PrimaryCullSources, .lines = &PointCullLines });
			// CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY observers.
			if (Diagnostics::Observe().claimsOrScene) {
				SubscribeLightParity();
				SubscribeFocusParity();
				SubscribeShadowVolumeParity();
			}
			// CS_DCLF_PERSISTENT_PARITY: the point lights' culls on frames that skip nothing.
			if (Diagnostics::Observe().scene)
				LightCulls::Install();
		}
	}

	Api::Feature Feature()
	{
		return { .name = "local shadows", .install = &Install, .sourceHooks = &SourceHooks, .preflight = &LocalLightCull::Preflight };
	}
}

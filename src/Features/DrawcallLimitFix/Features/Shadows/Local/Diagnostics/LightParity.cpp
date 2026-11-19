// The local shadow lights' views' parity (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY): the Lights board against the engine's views. Render thread.

#include "Features/DrawcallLimitFix/Features/Shadows/Local/Local.h"

#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ShadowViewTally.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

namespace DCLF::Features::Shadows::Local
{
	namespace
	{
		Reports::Window lightWindow;

		/** @brief Logs the light views' parity line every 300 frames with local views, and resets the tally. */
		void LightViewsLines()
		{
			auto& p = Diagnostics::ShadowViewTally(Diagnostics::ShadowView::Light);
			const std::uint64_t differ = p.viewProj + p.eye + p.viewport + p.casters + p.missing;
			logger::info("[DCLF] light views (T3a: DCLF's, from the cameras the local lights' UpdateCamera set up, against the engine's): {} views ({} matrices, {} eye, "
						 "{} viewport, {} caster volume differ), {} without DCLF's{}{}",
				p.views, p.viewProj, p.eye, p.viewport, p.casters, p.missing, differ ? " <- LIGHT VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
			p = {};
		}

		void LightFrame(const Diagnostics::On<Diagnostics::EnginePoint::LightViewsFrame>&)
		{
			if (lightWindow.Tick())
				LightViewsLines();
		}
	}

	void SubscribeLightParity()
	{
		Diagnostics::Subscribe<Diagnostics::EnginePoint::LightViewsFrame>(LightFrame);
	}
}

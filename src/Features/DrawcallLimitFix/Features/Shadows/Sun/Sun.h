#pragma once

// Shadows/Sun: the sun's shadow views, drawn by the shadow epoch. Registers the Sun source with its accumulation hooks
// (SunAccumulation.h) and camera hook (SunViews.h), which records the cascades and full-frustum planes into the board.
// The registration thunk also does main-registration work for Core and for Shadows/Local's point-light bits, so this feature depends
// on Shadows/Local. Without it the point lights' culls stay off. The exclusion it applies is built by Shadows/Local.
// The accumulation skip and entry exclusion are always on while DCLF draws the shadows. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Shadows::Sun
{
	Api::Feature Feature();

	void SunAccumulationLines(std::uint32_t a_frame);
}

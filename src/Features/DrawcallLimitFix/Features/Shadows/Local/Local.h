#pragma once

// Shadows/Local: the local shadow lights' views and the focus views, drawn by the shadow epoch.
// Registers the Spot, Point and Focus shadow sources, which record views into boards that Core reads through the sources' camera.
// Also owns the point lights' shadow culls (LocalLightCull.h) and the shadow payload's exclusion build (ExclusionBuild.h,
// SunExclusion.h). The build lives here because Features/Shadows/Sun depends on this feature, not the reverse.
// Core keeps the shadow engine's scene side: commit, captures, camera math, caster phases and candidates.
// Features/LocalLights owns the light selection. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Shadows::Local
{
	Api::Feature Feature();

	void PointCullLines(std::uint32_t a_frame);
}

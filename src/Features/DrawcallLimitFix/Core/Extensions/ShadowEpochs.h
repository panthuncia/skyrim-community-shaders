#pragma once

// Core's side of Api/ShadowEpochs.h. Without a contribution, FrameDriver's shadow points do only Core's bookkeeping
// and the engine draws every shadow view and occlusion map.

#include "Features/DrawcallLimitFix/Api/ShadowEpochs.h"

namespace DCLF::Extensions
{
	/** @brief Main thread, once, at the end of InstallFeatures. */
	void ResolveShadowEpochs(const Api::ShadowEpochsContribution& a_registered);
	/** @brief Any thread, after InstallFeatures. Null without a contribution. */
	const Api::ShadowEpochsContribution* ShadowEpochs();
}

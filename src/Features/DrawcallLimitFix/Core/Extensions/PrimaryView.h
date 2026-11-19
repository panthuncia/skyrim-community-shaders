#pragma once

// Core's side of Api/PrimaryView.h. Without a contribution, FrameDriver's main pass points do only Core's bookkeeping.

#include "Features/DrawcallLimitFix/Api/PrimaryView.h"

namespace DCLF::Extensions
{
	/** @brief Main thread, once, at the end of InstallFeatures. */
	void ResolvePrimaryView(const Api::PrimaryViewContribution& a_registered);
	/** @brief Any thread, after InstallFeatures. Null without a contribution. */
	const Api::PrimaryViewContribution* PrimaryView();
}

#pragma once

// MainView: what only the main view's draws use. Its draw passes (MainViewPasses.cpp) and the main pass's points (MainViewPoints.cpp).
// Core keeps what every view shares: the frame commits, BuildDraws, the latched copies, the index pool, the sorts and the revisions.
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::MainView
{
	Api::Feature Feature();

	void RegisterPasses(Api::InstallContext& a_context);

	void RegisterPrimaryView(Api::InstallContext& a_context);
}

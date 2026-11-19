#pragma once

// The frame's schedule. Only the shell includes this, handing it to Extensions::InstallFeatures. Core never includes Schedule/.

#include "Features/DrawcallLimitFix/Api/Epochs.h"

namespace DCLF::Schedule
{
	/** @brief Every epoch, its shape, and its passes' keys in order. */
	const Api::FrameSchedule& Frame();
}

#pragma once

// The scene observers registered by SceneInstall.cpp, and what one observer shares with another.

#include "SceneView.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <utility>

namespace DCLF::Diagnostics
{
	void InstallRegistrations(bool a_parity);
	void InstallCensus();
	void InstallPrecisionCensus();
	void InstallWalkParity();
	void InstallChangeLogParity();
	void InstallResidentParity();
	void InstallSlotDrainCheck();
	void InstallFrameParity();
	void InstallCandidateParity();
	void InstallShadowSetParity();
	void InstallReplayParity();
	void InstallMirrorReads();
	void InstallMirrorParity(bool a_switchable = false);
	void InstallCommitParity();
	void InstallMirrorWatchPlacement();

	/** @brief The mirror parity's lines since the last call, newline-terminated, or empty. */
	std::string TakeMirrorParityLines();

}

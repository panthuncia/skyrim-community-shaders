#pragma once

// Fades' diagnostics: report lines, plus the port check and FadeStateCS's log under CS_DCLF_FADE_PARITY.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeState.h"

#include <cstdint>
#include <vector>

namespace DCLF::Features::Fades
{
	void RegisterReport(Api::InstallContext& a_context);

	/** @brief Runs the port check and node snapshot every 30th frame. */
	void SubscribePortParity();
	/** @brief Render thread. The fade roots' nodes after the list jobs of a_frame, or null if no snapshot was taken then. */
	const std::vector<FadeNodeState>* NodeSnapshot(std::uint32_t a_frame);
	/** @brief Render thread. The port check's counts since the last call. */
	FadeState::PortCheck TakePortCheck();

	/** @brief Opens the log window at the depth commit and reads it back after the depth epoch. */
	void SubscribeFadeLog();
}

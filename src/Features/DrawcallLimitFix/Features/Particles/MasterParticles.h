#pragma once

// Master particle systems: torches, candles and street mist drawn by DCLF's particle path.
// The particle manager holds each BSMasterParticleSystem in its child array without being its parent. The masters are unheld roots,
// held by key alone, because the engine removes a master once its reference count drops below 3 and a DCLF reference would leak it.
// Lifetime comes from events: attach through AttachChild, removal by the manager's update (FUN_140e2a520, hooked here to push the
// detach first), the world reset's remove-all (FUN_140648cd0 +0x584), and DetachChild overrides.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <string>

namespace DCLF::Features::Particles::MasterParticles
{
	/** @brief Checks the removal call site at preflight, when enabled. Main thread. */
	void Preflight();

	/** @brief Registers the unheld roots contribution, the removal hook and the counters, when enabled. Main thread, at install. */
	void Install(Api::InstallContext& a_context);

	/** @brief The report's lines, under CS_DCLF_STATS. */
	void Lines(std::string& a_text);
}

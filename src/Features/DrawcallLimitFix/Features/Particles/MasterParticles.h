#pragma once

// The master particle systems (CS_DCLF_MASTER_PARTICLES, default off; dclf-roadmap.md 1.4): torches, candles and street mist
// (MPSTorchFire01.nif, MPSCandleFlame01.nif, MPSMistStreet01.nif, ...) drawn by DCLF's particle path. The particle manager
// (BSParticleSystemManager, ShadowSceneNode child 5) holds each BSMasterParticleSystem in its child array without being its parent.
// The masters are DCLF's unheld roots (Api::UnheldRootsContribution): category roots filed under themselves, held by key alone,
// because the engine removes a master once its reference count drops below 3 (a lasting DCLF reference would leak it).
//
// A master's lifetime comes from events:
// - its attach: the manager's AttachChild, SceneWriterFramework's detour (SceneTracker names the manager as the parent);
// - its removal by the manager's update (FUN_140e2a520, AE ID 76601 / SE 74853: the slot cleared through NiTObjectArray::SetAt at +0x69,
//   with no child edit): this feature's call hook pushes the detach before the slot clears;
// - the world reset (FUN_140648cd0 +0x584, AE ID 36601 / SE 35593 +0x475): the engine's remove-all on the manager, SceneWriterFramework's
//   remove-all detour, which fires a detach for every child;
// - the manager's own DetachChild overrides: SceneWriterFramework's detours.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <string>

namespace DCLF::Features::Particles::MasterParticles
{
	/** @brief Main thread, at DCLF's preflight: the removal call site, when enabled. */
	void Preflight();

	/**
	 * @brief Main thread, at the feature's install: the switch's listing; when enabled, the unheld roots contribution, the removal hook
	 * and the counters' subscriptions to the manager's child edits.
	 */
	void Install(Api::InstallContext& a_context);

	/** @brief The report's lines (the particles report, CS_DCLF_STATS; the observers behind them run only then): <- MASTER SET and <- MASTER REFS. */
	void Lines(std::string& a_text);
}

#pragma once

// Forward (CS_DCLF_FORWARD, CS_DCLF_REFRACTION): DCLF's blended objects, sorted back to front and drawn just before the
// engine's own blended draws, plus refraction geometry drawn into the refraction normals. Both run in the forward epoch.
// Core owns the parts other features share: the forward ranges, the sort, and the shared epoch's submission.
// See dclf-architecture.md, "The forward epoch".

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Forward
{
	Api::Feature Feature();

	void InstallHooks(Api::InstallContext& a_context);
	// Must run before any hook is written. Install all three hooks or none, because the refraction point undoes what the blended draw marks.
	void PreflightHooks();

	void RegisterPasses(Api::InstallContext& a_context);

	/**
	 * @brief Runs the forward epoch. Render thread, only after this frame's colour epoch has committed.
	 * Returns whether it drew the refraction normals, which the caller must then mark for the engine.
	 */
	bool SubmitForwardEpoch();
	/** @brief Whether this frame's forward epoch drew the refraction normals. Render thread. */
	bool RefractionDrawnThisFrame();
	void RegisterReport(Api::InstallContext& a_context);
}

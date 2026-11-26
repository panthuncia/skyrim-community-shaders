#pragma once

// Forward: DCLF's blended objects, sorted back to front and drawn just before the
// engine's own blended draws, plus refraction geometry drawn into the refraction normals. Both run in the forward epoch.
// Core owns the parts other features share: the forward ranges, the sort, and the shared epoch's submission.
// The CPU pipeline's forward site draws both, in either pipeline.
// See dclf-architecture.md, "The forward epoch".

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Forward
{
	Api::Feature Feature();

	void InstallHooks(Api::InstallContext& a_context);
	// Must run before any hook is written. Install all three hooks or none, because the refraction point undoes what the blended draw marks.
	void PreflightHooks();

	/**
	 * @brief Runs the forward epoch. Render thread, only after this frame's colour epoch has committed.
	 * Returns whether it drew the refraction normals, which the caller must then mark for the engine.
	 */
	bool SubmitForwardEpoch();

	/**
	* @brief Native's RenderWorld driver: the main pass's blended draw point (the post-resolve's, SE 99939 +0x54 / +0x551, AE 106584 +0x51
	* / +0x543), with no engine call. a_afterWater false is the call before the water when the camera is under water. Render owner.
	*/
	void NativeBlendedDraw(bool a_afterWater);

	/** @brief Whether this frame's forward epoch drew the refraction normals. Render thread. */
	bool RefractionDrawnThisFrame();
	void RegisterReport(Api::InstallContext& a_context);
}

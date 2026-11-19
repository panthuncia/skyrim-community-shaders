#pragma once

// Shadows/Engine: the shadow epoch, which draws the shadow views and the occlusion maps. Holds its passes, its frame points, and
// the hooks on the engine's shadow-light selection that reach FrameDriver. Shadows/Local, Shadows/Sun and OcclusionMaps build on it.
// Core keeps the scene side: the revision participant, shadow tables, caster verdicts, PassCapture withholding and the epoch's commit.
// This namespace hides DCLF::Engine, so sources here must name the engine access DCLF::Engine in full.
// Included by the feature's sources and the feature list only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Shadows::Engine
{
	Api::Feature Feature();

	void RegisterPasses(Api::InstallContext& a_context);

	void RegisterShadowEpochs(Api::InstallContext& a_context);

	void InstallCasterHooks();
	// Checks the casters' three calls before any hook is written. They must be CALLs, as the thunks chain.
	void PreflightCasterHooks();
}

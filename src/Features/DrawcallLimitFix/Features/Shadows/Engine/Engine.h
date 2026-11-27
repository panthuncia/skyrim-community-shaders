#pragma once

// Shadows/Engine: the shadow epoch, which draws the shadow views and the occlusion maps. Holds its passes, its frame points, and
// the hooks on the engine's shadow-light selection that reach FrameDriver. Shadows/Local, Shadows/Sun and OcclusionMaps build on it.
// Core keeps the scene side: the revision participant, shadow tables, caster verdicts, PassCapture withholding and the epoch's commit.
// This namespace hides DCLF::Engine, so sources here must name the engine access DCLF::Engine in full.
// Included by the feature's sources and the feature list only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace RE
{
	class BSShadowLight;
}

namespace DCLF::Features::Shadows::Engine
{
	Api::Feature Feature();

	void RegisterPasses(Api::InstallContext& a_context);

	void RegisterShadowEpochs(Api::InstallContext& a_context);

	void InstallCasterHooks();
	/** @brief Hooks the shadow lights' Render, so a light whose views DCLF draws whole skips it (ShadowMapsNative.cpp). */
	void InstallShadowMapHooks();
	/**
	 * @brief Render thread, inside the shadow maps: a_light's native shadow maps (its views captured, its Render's bookkeeping done),
	 * a_index advanced as its Render would. False when DCLF cannot draw its views: then nothing is drawn and nothing reaches the engine.
	 */
	bool RenderLightNatively(RE::BSShadowLight& a_light, std::uint32_t& a_index);
	/**
	 * @brief Render thread, in a native menu scene render: a_light's Render bookkeeping (each descriptor's target and slice, the slice's
	 * clear and its light transform), with no view captured and no camera state left behind. A frustum or paraboloid light only; the
	 * caller draws the maps. a_index advances as its Render would.
	 */
	void RenderMenuLight(RE::BSShadowLight& a_light, std::uint32_t& a_index);
	/** @brief Render thread. Set while Main::RenderShadowMaps runs, between DCLF's shadow map points. */
	void SetShadowMapsOpen(bool a_open);
	// Checks the casters' three calls before any hook is written. They must be CALLs, as the thunks chain.
	void PreflightCasterHooks();
}

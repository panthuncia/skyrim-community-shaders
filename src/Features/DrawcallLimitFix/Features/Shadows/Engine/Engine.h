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

namespace DCLF::Features::Shadows::Native
{
	struct ShadowLight;
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
	 * @brief Native, game side, once the caster selection is done: a_light's Render bookkeeping (each descriptor's target, slice and light
	 * transform, its scene accumulate array let go) and what its draws need into a_out (whether DCLF draws it, its views and slice clears,
	 * the state its last draw leaves). a_index advances as its Render would.
	 */
	void PrepareLightMaps(RE::BSShadowLight& a_light, Native::ShadowLight& a_out, std::uint32_t& a_index);
	/**
	 * @brief Native, render side, inside the shadow maps: a kept light's shadow maps from the light set (its views captured, its slices
	 * cleared). False when DCLF cannot draw its views: then nothing is drawn and nothing reaches the engine.
	 */
	bool RenderLightNatively(const Native::ShadowLight& a_light);
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

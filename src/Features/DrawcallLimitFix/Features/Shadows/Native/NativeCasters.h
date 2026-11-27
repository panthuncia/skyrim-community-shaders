#pragma once

// Native mode's shadow lights without the engine's shadow code: CS selects the shadow casters, sets up their cameras and does the
// bookkeeping the shadow maps and masks read, in place of Main::CalculateAndDrawShadowCasterLights (AE 107132, SE 100414) and
// Main::RenderShadowMaps (AE 107133, SE 100415). The main camera's list accumulation and registration jobs are not run: native draws
// nothing from the engine's scene lists or registered passes. The selection is game-side work; what the shadow maps, masks and lens
// flares read of it goes to the render side as the light set (LightSet.h).

#include <cstdint>

namespace RE
{
	class BSShadowLight;
	class NiCamera;
}

namespace DCLF::Features::Shadows::Native
{
	/** @brief Whether native mode replaces the engine's shadow light selection and shadow maps. Any thread, after setup. */
	bool Replaces();

	/**
	 * @brief Game side (the native driver's game half), in place of CalculateAndDrawShadowCasterLights: the sun's cameras and keep,
	 * then DCLF's light selection with the engine's per-light bookkeeping. The primary cull's frame ends here, where the list jobs
	 * were awaited. CaptureSet follows once DCLF listed the views.
	 */
	void SelectCasters();

	/**
	 * @brief Render side, in place of Main::RenderShadowMaps: each kept light's native shadow maps, from the adopted light set. A light
	 * whose views DCLF cannot draw casts no shadow; nothing reaches the engine's Render. The main camera's registration jobs are not run.
	 */
	void RenderShadowMaps();

	/**
	 * @brief Render side, at the shadow maps' start (before DCLF's BeforeShadowMaps): the native driver's render half adopts its set's
	 * shadow view list, sets this frame's capacity, decides whether DCLF draws the views and covers them. Nothing otherwise.
	 */
	void AdoptShadowViews();

	/**
	 * @brief Render side: whether this frame's RenderShadowMaps drew a_light's maps. A kept light it could not draw casts no shadow, so the
	 * masks leave its channel lit; its descriptors may hold no target yet (a light first kept on a load's first world frame).
	 */
	bool DrewMaps(const RE::BSShadowLight* a_light);

	/** @brief The engine's UpdateCamera for any shadow light class, by CS's ports. Returns whether the light is visible. */
	bool UpdateLightCamera(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera);

	/** @brief A light not kept: its portal visibility cleared and its shadow maps returned (LocalCameras.h). */
	void ReleaseLight(RE::BSShadowLight& a_light);

	/** @brief The frame's focus host: its focus cameras, DCLF's focus views and its focus accumulators, as the engine's selection does. */
	void FocusHost(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera);

	/** @brief A kept local light: its shadowLightsAccum slot, mask index, scissor rectangle, Accumulate bookkeeping and lens flare. */
	void KeepLocal(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera, std::uint32_t a_kept);
}

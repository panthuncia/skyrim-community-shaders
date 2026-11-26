#pragma once

// Native mode's shadow lights without the engine's shadow code: CS selects the shadow casters, sets up their cameras and does the
// bookkeeping the shadow maps and masks read, in place of Main::CalculateAndDrawShadowCasterLights (AE 107132, SE 100414) and
// Main::RenderShadowMaps (AE 107133, SE 100415). The engine's scene-list work these two functions also run (the main camera's list
// accumulation and registration jobs) still runs from here until the scene lists are DCLF's.

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
	 * @brief Render thread, in place of CalculateAndDrawShadowCasterLights(a_camera): the sun's cameras and keep, the main camera's list
	 * accumulation jobs (engine work), and DCLF's light selection with the engine's per-light bookkeeping while they run.
	 */
	void SelectCasters(RE::NiCamera* a_camera);

	/**
	 * @brief Render thread, in place of Main::RenderShadowMaps: the main camera's registration jobs (engine work) and each kept light's
	 * native shadow maps. A light whose views DCLF cannot draw casts no shadow; nothing reaches the engine's Render.
	 */
	void RenderShadowMaps();

	/** @brief The engine's UpdateCamera for any shadow light class, by CS's ports. Returns whether the light is visible. */
	bool UpdateLightCamera(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera);

	/** @brief A light not kept: its portal visibility cleared and its shadow maps returned (LocalCameras.h). */
	void ReleaseLight(RE::BSShadowLight& a_light);

	/** @brief The frame's focus host: its focus cameras, DCLF's focus views and its focus accumulators, as the engine's selection does. */
	void FocusHost(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera);

	/** @brief A kept local light: its shadowLightsAccum slot, mask index, scissor rectangle, Accumulate bookkeeping and lens flare. */
	void KeepLocal(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera, std::uint32_t a_kept);
}

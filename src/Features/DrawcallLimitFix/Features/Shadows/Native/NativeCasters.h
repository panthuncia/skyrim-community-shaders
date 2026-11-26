#pragma once

// Native mode's shadow lights without the engine's shadow code: CS selects the shadow casters, sets up their cameras and does the
// bookkeeping the shadow maps and masks read, in place of Main::CalculateAndDrawShadowCasterLights (AE 107132, SE 100414) and
// Main::RenderShadowMaps (AE 107133, SE 100415). The main camera's list accumulation and registration jobs are not run: native draws
// nothing from the engine's scene lists or registered passes.

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
	 * @brief Render thread, in place of CalculateAndDrawShadowCasterLights: the sun's cameras and keep, then DCLF's light selection with
	 * the engine's per-light bookkeeping. The primary cull's frame ends here, where the list jobs were awaited.
	 */
	void SelectCasters();

	/**
	 * @brief Render thread, in place of Main::RenderShadowMaps: each kept light's native shadow maps. A light whose views DCLF cannot
	 * draw casts no shadow; nothing reaches the engine's Render. The main camera's registration jobs are not run.
	 */
	void RenderShadowMaps();

	/**
	 * @brief Render thread: whether this frame's RenderShadowMaps drew a_light's maps. A kept light it could not draw casts no shadow,
	 * so the masks leave its channel lit; its descriptors may hold no target yet (a light first kept on a load's first world frame).
	 */
	bool DrewMaps(const RE::BSShadowLight& a_light);

	/** @brief The engine's UpdateCamera for any shadow light class, by CS's ports. Returns whether the light is visible. */
	bool UpdateLightCamera(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera);

	/** @brief A light not kept: its portal visibility cleared and its shadow maps returned (LocalCameras.h). */
	void ReleaseLight(RE::BSShadowLight& a_light);

	/** @brief The frame's focus host: its focus cameras, DCLF's focus views and its focus accumulators, as the engine's selection does. */
	void FocusHost(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera);

	/** @brief A kept local light: its shadowLightsAccum slot, mask index, scissor rectangle, Accumulate bookkeeping and lens flare. */
	void KeepLocal(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera, std::uint32_t a_kept);
}

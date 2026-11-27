#pragma once

// Ports of the per-light work CalculateActiveShadowCasterLights (AE 0x1414cc570, SE 0x1412e2f60) does for each shadow light, so
// native mode can select the shadow lights itself. Each function is a drop-in for the engine function it names: same inputs, and the
// same writes to the light, its cameras, accumulators, culling process and the engine's globals. Inside the light selection: the
// native driver's game half for the world, a menu scene's render for its lights. The float math keeps the engine's operation order.

#include <cstdint>

namespace RE
{
	class BSShadowLight;
	class BSShadowFrustumLight;
	class BSShadowParabolicLight;
	class NiCamera;
}

namespace DCLF::Features::Shadows::Native
{
	/**
	 * @brief BSShadowFrustumLight::UpdateCamera (vtable slot 0x10; AE 0x14151ac50, SE 0x14132d1d0). Moves a dynamic light's camera to
	 * the light, makes the accumulator on first use, and tests the light's frustum against the view camera's. Writes frustrumCull
	 * (+0x5C) and lodDimmer. Returns whether the light is visible.
	 */
	bool UpdateFrustumCamera(RE::BSShadowFrustumLight& a_light, const RE::NiCamera& a_viewCamera);

	/**
	 * @brief BSShadowParabolicLight::UpdateCamera (vtable slot 0x10; AE 0x14151b620, SE 0x14132ddf0). Moves a dynamic light's two
	 * hemisphere cameras to the light and tests its sphere against the view camera's planes. Returns whether the light is visible.
	 */
	bool UpdateParabolicCamera(RE::BSShadowParabolicLight& a_light, const RE::NiCamera& a_viewCamera);

	/**
	 * @brief The focus host's focus cameras (AE FUN_1414f0480, SE FUN_141304da0, Ghidra's "BSShadowDirectionalLight::sub"). One
	 * orthographic camera per focus target, looking along the light's first camera, sized to the target's bound, with a port that
	 * shrinks with the target's distance from a_worldCamera. Makes the cameras on first use.
	 */
	void UpdateFocusCameras(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera);

	/**
	 * @brief NiCamera's sphere projection (AE FUN_140d2bab0, SE FUN_140c65a20). Projects a sphere (centre, radius) with the camera.
	 * Writes the bounds' minimum and maximum: x and y in [-1, 1] (the whole range when a point is behind a_epsilon), z in [0, 1].
	 */
	void ProjectSphere(const RE::NiCamera& a_camera, const float (&a_sphere)[4], float (&a_min)[3], float (&a_max)[3], float a_epsilon);

	/**
	 * @brief The scissor rectangle CalculateActive computes for a kept light. Projects the light's sphere with the world camera into
	 * projectedBoundingBox (+0x544..+0x550, in screen pixels). With the camera inside the sphere the rectangle is the whole screen, and
	 * the bit a_shadowLightCount (gLastFrameEnabledShadowCasterLightsCount2 before the light's Accumulate) is set in the engine's
	 * camera-inside mask.
	 */
	void ProjectLightRect(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera, std::uint32_t a_shadowLightCount);

	/**
	 * @brief Vtable slot 4, GetIsFrustumOrDirectionalLight. A constant per class: true for BSShadowFrustumLight and
	 * BSShadowDirectionalLight, false for BSShadowParabolicLight (BSShadowLight's).
	 */
	bool IsFrustumOrDirectional(const RE::BSShadowLight& a_light);

	/**
	 * @brief Vtable slot 0x0C, ReturnShadowmaps (BSShadowLight AE 0x1414f0410 / SE 0x141304d30, BSShadowParabolicLight AE 0x14151bde0
	 * / SE 0x14132e590, BSShadowDirectionalLight AE 0x141511ed0 / SE 0x141324da0). Gives the light's shadow map slices back.
	 */
	void ReturnShadowmaps(RE::BSShadowLight& a_light);

	/**
	 * @brief CalculateActive's branch for a light it does not keep: clears the light's portal entry visibility, then returns its
	 * shadow maps.
	 */
	void ReleaseLight(RE::BSShadowLight& a_light);

	/**
	 * @brief The bookkeeping of Accumulate (vtable slot 9; BSShadowFrustumLight AE 0x14151aa60 / SE 0x14132cfd0,
	 * BSShadowParabolicLight AE 0x14151b960 / SE 0x14132e140, BSShadowDirectionalLight AE 0x141511c80), without its cull and pass registration. Sets the first accumulator's
	 * light index (+0x160) and mask bit (+0x164), gives a two-sided paraboloid's culling process its back accumulator, and advances
	 * a_globalCount by the light's views. a_maskChannel only feeds the cull, so it is unused here. Other classes do nothing.
	 */
	void AccumulateBookkeeping(RE::BSShadowLight& a_light, std::uint32_t& a_globalCount, std::uint32_t a_maskChannel);

	/**
	 * @brief The focus accumulation (AE FUN_1414f0b90, SE FUN_1413054c0) without its registration: each focus descriptor enabled, its
	 * accumulator made on first use (render mode 0xE, no decals) and pointed at the main shadow scene node. DCLF draws the focus views
	 * from its own tables, so nothing is registered.
	 */
	void PrepareFocusAccumulators(RE::BSShadowLight& a_light);
}

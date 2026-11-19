#pragma once

// Cull volumes of the frame's local shadow lights. BuildDraws tests each object against them to build its main-pass shadow mask.
// Core owns the test, which mirrors BuildDrawsCS. The feature supplies the lights and their volumes. One contribution only.
// Without Light Limit Fix, the same contribution also supplies the frame's point light table.

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/LightVolumes.h.
	class LightVolumeSink;

	/**
	 * @brief One cull volume of a local shadow light: the light's sphere, then one shadowmap descriptor's planes.
	 * A light with several descriptors adds one volume per descriptor, all with the same mask bit.
	 *
	 * - planes: inside where dot(normal, p) - constant >= 0. 0-5 are the frustum and 6-11 the custom planes.
	 * - masks: the active planes in each group of six.
	 */
	struct LightVolume
	{
		std::uint32_t maskBit = 0;
		bool affectsLand = false;
		float sphere[4]{};
		float planes[12][4]{};
		std::uint32_t masks[2]{};
		// A portal-strict point light's light context atlas entry plus 1. Receivers must pass its strict gates. 0 means not gated.
		std::uint32_t portalContext = 0;
	};

	/**
	 * @brief An entry in the frame's point light table, used by Lighting draws when Light Limit Fix is not loaded.
	 * Holds what the engine's SetupGeometry (FUN_1414df650) writes to PointLightPosition and PointLightColor.
	 *
	 * - color: NiLight diffuse times its fade and the BSLight's lodDimmer.
	 * - shadowMaskIndex: ~0u for a plain light.
	 * - affectsLand: BSLight +0x61.
	 */
	struct PointLight
	{
		float position[3]{};
		float radius = 0.0f;
		float color[3]{};
		std::uint32_t shadowMaskIndex = ~0u;
		bool affectsLand = false;
	};

	/**
	 * @brief The light volumes contribution.
	 *
	 * - landscapeFlags: objects with these property flags take only affectsLand volumes.
	 * - sample: render thread, once per colour commit. Volumes go in the order their mask bits combine.
	 * - pointLightsActive: whether Lighting draws read the point light table. Constant for the session.
	 * - pointLights: shadow lights first, in mask bit order, then plain lights. Returns how many were written.
	 */
	struct LightVolumesContribution
	{
		std::string_view name;
		std::uint64_t landscapeFlags = 0;
		void (*sample)(LightVolumeSink& a_sink) = nullptr;
		bool (*pointLightsActive)() = nullptr;
		std::uint32_t (*pointLights)(PointLight* a_out, std::uint32_t a_capacity) = nullptr;
	};
}

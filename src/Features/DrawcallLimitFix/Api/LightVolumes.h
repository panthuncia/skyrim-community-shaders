#pragma once

// The light volumes contract (Api::InstallContext::RegisterLightVolumes): the cull volumes of the frame's local shadow lights, which every
// colour commit's BuildDraws tests each object against for its main-pass shadow mask (Light Limit Fix's ShadowBitMask, kObjectLocalShadow*).
// The test, the latch region and its clamp are Core's (they mirror BuildDrawsCS); which lights there are and their volumes are the
// feature's (Features/LocalLights: the light selection's lights and the Lights board's cameras). One contribution: a second fails the
// install. Without one no volume is sampled and no property takes the landscape rule. The same contribution gives the frame's point
// light table while Light Limit Fix is not loaded (PointLight): the shadow lights are the volumes' lights.

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/LightVolumes.h), which the features inside DCLF include for it.
	class LightVolumeSink;

	/**
	 * @brief One cull volume of a local shadow light, as BuildDraws tests it: the light's sphere, then a shadowmap descriptor's planes. A
	 * light of several descriptors puts a volume each, with the same mask bit.
	 *
	 * - maskBit: the light's bit of the object's mask (1 << its mask index).
	 * - affectsLand: the volume is taken by the landscape properties too (LightVolumesContribution::landscapeFlags).
	 * - sphere: the light's centre and radius (its accumulation walks only the scene around it).
	 * - planes: (normal, constant), inside where dot(normal, p) - constant >= 0: the frustum (0-5), the custom planes (6-11).
	 * - masks: the active planes of each six (frustum, custom).
	 */
	struct LightVolume
	{
		std::uint32_t maskBit = 0;
		bool affectsLand = false;
		float sphere[4]{};
		float planes[12][4]{};
		std::uint32_t masks[2]{};
		// A portal-strict point light's entry in the frame's light context atlas + 1 (the cull binding LightContexts), whose strict gates a
		// receiver must pass. 0: not gated.
		std::uint32_t portalContext = 0;
	};

	/**
	 * @brief A point light of the frame's point light table (Common/DCLFPointLights.hlsli), which the Lighting draws pick their per-object
	 * lights from while Light Limit Fix is not loaded: the values the engine's SetupGeometry writes to PointLightPosition and
	 * PointLightColor (FUN_1414df650).
	 *
	 * - position, radius: the light's world position and radius (its NiLight's radius.x).
	 * - color: its NiLight's diffuse times its fade and the BSLight's lodDimmer.
	 * - shadowMaskIndex: a shadow light's mask index (its volumes' maskBit is 1 << it), ~0u for a plain light.
	 * - affectsLand: a plain light the landscape properties take (BSLight +0x61).
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
	 * - name: the contribution's (the install's failures).
	 * - landscapeFlags: the property flags whose objects take only affectsLand volumes (kObjectLandscapeLights), read as data per join.
	 * - sample: render thread, once per colour commit (CommitShadowInputs), after the lights accumulated: every volume of the frame's
	 *   lights into a_sink, in the order their mask bits combine. Core keeps the list
	 *   unclamped (the build parity's reference) and latches at most the revision's layout's volumes.
	 * - pointLightsActive: any thread, after the install: whether the Lighting draws read the frame's point light table (Light Limit Fix
	 *   not loaded: its clustered lights light them otherwise). Constant for the session. Null: never.
	 * - pointLights: render thread, once per commit while pointLightsActive: the frame's point lights into a_out, at most a_capacity, the
	 *   shadow lights first in the order their mask bits combine, then the plain lights; returns how many it wrote.
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

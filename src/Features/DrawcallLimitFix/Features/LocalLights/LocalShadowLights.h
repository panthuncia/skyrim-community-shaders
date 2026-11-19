#pragma once

#include "Features/DrawcallLimitFix/Api/LightVolumes.h"

#include <array>
#include <cstdint>
#include <vector>

namespace RE
{
	class BSShaderProperty;
}

namespace DCLF
{
	/**
	 * @brief The frame's local spot and point shadow lights, as the main pass's light selection (FUN_1414fcf80) sees them.
	 * An object takes a light when that light's cull kept it, except landscape objects skip lights whose +0x61 is clear.
	 * Light Limit Fix's ShadowBitMask is the OR of the taken lights' maskIndex bits.
	 */
	struct LocalShadowLights
	{
		struct Light
		{
			std::uint32_t maskBit = 0;  // 1 << maskIndex
			bool affectsLand = false;   // +0x61
			// The light's accumulation only walks the scene inside this sphere.
			float center[3]{};
			float radius = 0.0f;
			// Per shadowmap descriptor. Inside is dot(normal, p) - constant >= 0. Only the first six planes are used.
			struct Volume
			{
				std::array<std::array<float, 4>, 12> planes{};
				std::uint32_t masks[2]{};
			};
			std::vector<Volume> volumes;
			// Atlas entry + 1 for a portal-strict point light, whose receivers must pass its strict gates. 0 means not gated.
			std::uint32_t portalContext = 0;
		};
		std::vector<Light> lights;

		/** @brief Render thread, after the lights have accumulated. */
		static LocalShadowLights Sample();

		/**
		 * @brief The point light table used without Light Limit Fix, in SetupGeometry's order (FUN_1414df650): shadow lights first,
		 * then active lights that are neither frustum-culled (+0x5C 0xFF) nor hidden. Render thread, after the lights have accumulated.
		 */
		static std::uint32_t SamplePointLights(Api::PointLight* a_out, std::uint32_t a_capacity);
	};
}

#pragma once

#include <array>
#include <cstdint>

namespace DCLF::Diagnostics
{
	/** @brief An object's local shadow lights as BuildDrawsCS last wrote them into its colour draw. */
	struct DrawnLocalShadows
	{
		std::uint32_t mask = 0;   // 1 << the light's mask index
		std::uint32_t gated = 0;  // the portal-strict lights' bits, decided by StrictGateRejects
		float bound[4]{};         // world bound when armed, a receiver that moved since may differ
		std::array<const void*, 4> channels{};  // each channel's light when armed
	};

	/** @brief a_object's lights from the newest build parity readback. Three main epochs old, so compare in a still scene. Render thread. */
	bool DrawnLocalShadowsOf(std::uint32_t a_object, DrawnLocalShadows& a_out);

	/**
	 * @brief Each local shadow mask channel's light now, by mask index at BSShadowLight +0x520. Render thread.
	 * Channels are reassigned as the lights in view change, so an older readback names its bits by other lights.
	 */
	std::array<const void*, 4> ChannelLights();
}

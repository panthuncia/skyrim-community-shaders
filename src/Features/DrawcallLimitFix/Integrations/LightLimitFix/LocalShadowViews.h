#pragma once

// Light Limit Fix's local shadow cache follows the shadow views DCLF's shadow epoch draws. This is what it reads of them.
// Light Limit Fix includes it without the rest of DCLF's API.

#include <cstdint>
#include <vector>

namespace RE
{
	class BSShadowLight;
}

namespace DCLF::Integrations::LightLimitFix
{
	/** @brief A local spot or point light's first shadow view that DCLF's shadow epoch draws this frame. */
	struct LocalShadowView
	{
		const RE::BSShadowLight* light = nullptr;
		std::uint32_t target = 0;  // RE::RENDER_TARGETS_DEPTHSTENCIL, kSHADOWMAPS for every local light
		std::uint32_t slice = 0;   // the array slice the view draws into
	};

	/**
	 * @brief Render thread, from Main::RenderShadowMaps' return (EarlyPrepass) to the next frame's light selection.
	 * The local lights whose first view DCLF's shadow epoch draws this frame, once each, in the engine's order. Focus views,
	 * views the engine draws itself, and lights past their first descriptor are left out. Empty when DCLF draws no shadow
	 * views this frame or its selection has not run this frame.
	 */
	void DrawnLocalShadowViews(std::vector<LocalShadowView>& a_out);
}

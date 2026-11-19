#pragma once

// Frame points that build the frame's shadow view list on the render thread.
// They run while the engine picks its shadow lights in NiCamera::CalculateAndDrawShadowCasterLights, and at BeforeShadowMaps.

#include <cstdint>

namespace RE
{
	class BSShadowLight;
}

namespace DCLF::Draws
{
	/** @brief Call before the engine keeps its first shadow light. */
	void BeginShadowViews();
	/** @brief The engine kept a_light in the slots from a_firstSlot and accumulates it next. */
	void ShadowLightKept(const RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots);
	/** @brief Call before (a_begin true) and after the engine's focus accumulation of a_light. Draws registered in between belong to its focus views. */
	void ShadowFocusAccumulating(const RE::BSShadowLight* a_light, bool a_begin);
	/** @brief Call after the engine's light selection. Lists the shadow views in the order the engine renders them. */
	void ListShadowViews();
	/** @brief Call at BeforeShadowMaps. */
	void NoteShadowRevisionInputs();
}

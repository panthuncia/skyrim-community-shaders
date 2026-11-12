#pragma once

// The shadow views' frame points, Core's half (render thread), as FrameDriver calls them while the engine selects its shadow lights
// (NiCamera::CalculateAndDrawShadowCasterLights: Features/Shadows/Engine's hooks) and at BeforeShadowMaps: the frame's shadow views
// begun, each light's views as the engine keeps it, the list from DCLF's selection, and the revision inputs it moves. The scene side's:
// the capture attributes and withholds by this list. The epochs' half (the views' coverage, the shadow frame, the shadow and occlusion
// epochs, the maps' readiness) is Features/Shadows/Engine's, which FrameDriver calls through Api/ShadowEpochs.h. FrameDriver names
// nothing of the shadow engine behind it (Core/Render/Shadow: ShadowPoints.cpp).
// Self-contained.

#include <cstdint>

namespace RE
{
	class BSShadowLight;
}

namespace DCLF::Draws
{
	/** @brief Before the engine keeps its first shadow light: the frame's view capacity, and the list begun (ShadowViews::BeginCasters). */
	void BeginShadowViews();
	/** @brief The engine kept a_light in a_slots slots from a_firstSlot and accumulates it next: its views (ShadowViews::AddCaster). */
	void ShadowLightKept(const RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots);
	/**
	 * @brief Around the engine's focus accumulation of a_light (a_begin: before it, else after): its focus views listed and covered first
	 * (ShadowViews::AddFocusHost), and the registrations inside it taken as the covered focus views' (PassCapture::SetFocusAccumulation).
	 */
	void ShadowFocusAccumulating(const RE::BSShadowLight* a_light, bool a_begin);
	/**
	 * @brief The engine's selection done (CalculateAndDrawShadowCasterLights returned): the frame's shadow views, in the order the engine
	 * renders them at the shadow draw (ShadowViews::Rebuild).
	 */
	void ListShadowViews();
	/** @brief BeforeShadowMaps: the revision inputs the listed views move (their candidates). */
	void NoteShadowRevisionInputs();
}

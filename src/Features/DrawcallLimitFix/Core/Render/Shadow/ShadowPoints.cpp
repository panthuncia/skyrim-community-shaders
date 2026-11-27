#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowPoints.h"

#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF
{
	/**
	 * @brief Whether DCLF draws this frame's shadow views. It is decided before the engine's first shadow map clear,
	 * and the clears go into DCLF's shadow epoch only when this is true. Render thread.
	 */
	bool DrawsShadowViews()
	{
		return ShadowViews::Get().DclfDraws();
	}
}

namespace DCLF::Draws
{
	void BeginShadowViews()
	{
		// Set the capacity before any shadow light registers, so every later check sees this frame's value. Native's game half only
		// lists the views: the render side sets the capacity and covers its copy (ShadowViews::AdoptList).
		if (!Scene::FrameView::InGameHalf())
			ShadowViews::Get().SetViewCapacity(IndirectDraws::Get().ShadowViewCapacity());
		ShadowViews::Get().BeginCasters();
	}

	void ShadowLightKept(const RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots)
	{
		ShadowViews::Get().AddCaster(a_light, a_firstSlot, a_slots);
	}

	void ShadowFocusAccumulating(const RE::BSShadowLight* a_light, bool a_begin)
	{
		if (!a_begin) {
			PassCapture::SetFocusAccumulation(false);
			return;
		}
		PassCapture::SetFocusAccumulation(ShadowViews::Get().AddFocusHost(a_light));
	}

	void ListShadowViews()
	{
		// Everything downstream identifies a shadow view by its index in this list.
		ShadowViews::Get().Rebuild();
	}

	void NoteShadowRevisionInputs()
	{
		// The shadow candidates are a scene revision input, because the main latch holds one cascade and one shadow volume.
		IndirectDraws::Get().NoteRevisionInputs();
	}
}

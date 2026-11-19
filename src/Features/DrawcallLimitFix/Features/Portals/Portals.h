#pragma once

// Portals: DCLF's port of the main camera's portal and occluder walk (PortalViews), Main::IsRoomVisible answered from it
// (RoomVisibility), and their parity. The feature is the main cull input. Its programs buffer is uploaded with the depth commit and
// read by FadeStateCS and BuildDraws' phase 1.
// Its early hooks thunk Main::Update's call of FUN_1401a43c0. The primary cull requires this, so without this feature it stays off.
// Features/LocalLights calls PortalViews directly for per-light walks. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Portals
{
	Api::Feature Feature();

	void RegisterPrograms(Api::InstallContext& a_context);
	int PortalTest(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float a_centre[3], float a_radius,
		std::uint32_t a_nodeFlags, std::string& a_why);

	// Each self-scheduled every 300 builds or published VisibleSets.
	void PortalViewsLine();
	void RoomVisibilityLines();

	void InstallPortalCapture(Api::InstallContext& a_context);
}

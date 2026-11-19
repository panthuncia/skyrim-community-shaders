#pragma once

#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"

namespace DCLF::Diagnostics
{
	/** @brief Read-only access to LightSelection's private state for the parity observer. Render thread. */
	struct LightSelectionView
	{
		static const LightSelection& Self() { return LightSelection::Get(); }
		static const LightSelection::Frame& Frame() { return Self().frame; }
		static const auto& LightRooms() { return Self().lightRooms; }
		static const auto& RoomVerdicts() { return Self().roomVerdicts; }
		static const auto& FocusFlags() { return Self().focusFlags; }
		static const RE::BSShadowLight* LastFocusHost() { return Self().lastFocusHost; }
	};
}

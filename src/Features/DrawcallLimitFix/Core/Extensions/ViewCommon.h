#pragma once

// Core's side of Api/ViewCommon.h: the phases and view bits of every feature view in both families, checked and resolved once at install.

#include "Features/DrawcallLimitFix/Api/ViewCommon.h"

#include <array>
#include <cstdint>

namespace DCLF::Extensions
{
	/** @brief The phases and view bits of both families, and what Core derives from them. */
	struct ViewBitTable
	{
		std::uint8_t phases = 0;     // both families
		std::uint32_t viewBits = 0;  // both families
		// The view bits each set phase bit takes part in, including Core's main and caster phases.
		std::array<std::uint32_t, 8> viewBitsOfPhaseBit{};
		std::array<std::uint32_t, 256> viewBitsOfPhases{};
		// The phases FrameView's lacking counts use, in order: sky map, precipitation map, LodTechniques views. A map nobody takes is 0.
		std::array<std::uint8_t, 3> lackingPhases{};
		std::uint8_t lackingMask = 0;
	};

	namespace Detail
	{
		// Written only by ResolveViewBits on the main thread before DCLF's threads exist. Read-only after, so plain reads are safe.
		inline ViewBitTable resolvedViewBits;
	}

	/** @brief Any thread, after InstallFeatures. */
	inline const ViewBitTable& ResolvedViewBits() { return Detail::resolvedViewBits; }

	/**
	 * @brief Main thread, once, after ResolveViews and ResolveShadowViews. Builds the table.
	 * Fails the install for a phase or view bit that is empty, has several bits, or clashes with Core's or another view's.
	 */
	void ResolveViewBits();
}

#pragma once

// The first-person view's own frame state. Core keeps the rest of the view's slot in IndirectDraws::ViewState.

// ViewEpoch.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ViewEpoch.h"
#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"
#include "Features/DrawcallLimitFix/Api/Views.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace DCLF::Features::FirstPerson
{
	inline constexpr Api::EngineView kView = Api::EngineView::FirstPerson;

	struct FirstPersonFrame
	{
		std::uint32_t slots = 0, pipelinesReady = 0;
		std::vector<std::uint8_t> slotsWanted;
		std::uint32_t slotsFrame = 0;
		bool latch = false;
		std::uint32_t latchFrame = ~0u;
		// World space. previous is last frame's camera, for the motion vectors.
		std::array<std::byte, Draws::kViewBlockBytes> perFrame{};
		std::array<float, 16> viewProj{};
		RE::NiPoint3 eye;
		Draws::PreviousCamera previous;
		bool havePrevious = false;
		// The frame whose depth epoch built the sequences the colour epoch draws.
		std::uint32_t depthFrame = ~0u;
		// Since the last report.
		std::uint64_t frames = 0, latched = 0, depthEpochs = 0, colourEpochs = 0;
		std::array<std::uint64_t, 7> skipped{};  // not drawn (latch, claims), not covered, no camera, stale inputs, no resources, failed, no depth epoch
		std::uint64_t forwardMembers = 0;
		std::uint32_t forwardMembersNow = 0;
	};

	/** @brief Render thread. The report reads and resets its counters. */
	inline FirstPersonFrame& FrameState()
	{
		static FirstPersonFrame frame;
		return frame;
	}
}

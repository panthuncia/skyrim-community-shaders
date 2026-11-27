#pragma once

// FrameTicket: what DCLF's frame start took of the game, once per game update, for the render half.

#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF
{
	struct FrameGlobals;
	namespace LatePoses
	{
		struct FramePlan;
	}

	/**
	 * @brief The game half's product (FrameDriver::GameFrameStart): everything DCLF's frame start reads or writes of the engine, taken once
	 * per game update on the game thread. The render half (FrameDriver::BeginFrame) reads only the ticket, the snapshot and CS's frame.
	 * A repeated snapshot renders its ticket again. A ticket no render took retires unused: its one-shot inputs go to the next ticket.
	 * Retired through the snapshot retire queue's engine lane, like the snapshot it pairs with.
	 */
	struct FrameTicket
	{
		/** @brief What the first render of the ticket hands to the frame values once. A ticket never rendered hands them to the next one. */
		struct OneShot
		{
			std::vector<std::shared_ptr<const PlacementPlan>> plans;
			std::vector<ShadingItem> shading;
			std::vector<ShadingFieldValue> fields;
			// Sampled before the first-person shift. A newer ticket samples the movers again; its samples come after these.
			std::shared_ptr<FrameValues::RootViews> rootViews;
		};

		FrameTicket() = default;
		FrameTicket(const FrameTicket&) = delete;
		FrameTicket& operator=(const FrameTicket&) = delete;
		~FrameTicket() { delete oneShot.load(std::memory_order_relaxed); }

		/** @brief Any thread. Takes the one-shot inputs, null once taken. The first render takes them, or the next ticket when none did. */
		std::unique_ptr<OneShot> TakeOneShot() const { return std::unique_ptr<OneShot>(oneShot.exchange(nullptr, std::memory_order_acq_rel)); }

		std::uint64_t snapshot = 0;     // the snapshot it was made for (Snapshot::CurrentId at the game half)
		std::uint32_t sceneStep = 0;    // the scene step its game half began (a repeated render moves on)
		std::uint64_t publication = 0;  // the installed publication's sequence, 0 for none
		bool loading = false;           // a loading screen is up (FrameView::IsLoadingScreenUp)
		bool utilityShader = false;     // the engine's utility shader exists (its shadow techniques)
		std::shared_ptr<const FrameGlobals> globals;
		std::shared_ptr<const LatePoses::FramePlan> latePlan;  // every render of the ticket uses it
		std::array<std::shared_ptr<const void>, kTicketParts> parts;  // the features' values (RegisterTicketPart)
		mutable std::atomic<OneShot*> oneShot{ nullptr };
	};
}

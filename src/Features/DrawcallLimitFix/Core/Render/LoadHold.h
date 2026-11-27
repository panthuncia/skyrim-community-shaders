#pragma once

// The hold after every load. The loading screen stays up until DCLF draws the load's objects, so nothing pops in.
// The load coordinator (SE 13214 / AE 13363) ends a load by posting the loading menu's close at +0x211. The hold keeps that
// message, so the menu stays open, keeps presenting and pauses the game while the world's frames run under it.
// A load with no loading menu, or a re-entry such as after a coc or a closed map, holds the presents instead.
// While it holds, passes of DCLF's classes are withheld and the engine's culls of views DCLF does not cover yet are skipped.
// It ends after two settled frames in a row, or after a stall or a cap, and never hands a frame back to the engine.
//
// Install and the hooks run on the main thread. Active and MenuHeld on any thread. The rest on the render thread.

#include <cstdint>
#include <string_view>

namespace Snapshot
{
	struct FrameSnapshot;
}

namespace DCLF::LoadHold
{
	/** @brief Main thread, PostPostLoad. Installs the load coordinator hooks. A site another plugin changed is left alone and logged. */
	void Install();

	/** @brief Any thread. Whether a hold is in effect. */
	bool Active();
	/** @brief Any thread. Whether a hold is keeping the loading menu open. */
	bool MenuHeld();
	/** @brief Render thread, at Present. Whether to skip the present and keep the previous frame on screen. */
	bool PresentsHeld();

	enum class View : std::uint8_t
	{
		Sun,
		Occlusion,
		Shadow  // DCLF cannot draw them this frame
	};
	/** @brief Render thread. Notes that a view's engine cull was skipped because DCLF does not cover it yet. */
	void NoteHeld(View a_view);

	/**
	 * @brief Starts a hold with the presents held, for a camera cut the set was not culled for (the map menu, from the game half) or a
	 * view DCLF cannot draw (the render half). a_frame is the snapshot of the half that asks: Snapshot::Building() in the game half,
	 * Snapshot::Current() in the render half.
	 */
	void BeginReentry(const char* a_why, const Snapshot::FrameSnapshot* a_frame);

	/**
	 * @brief Render thread. DCLF should draw a_view's shadow or occlusion views this frame but cannot. During a hold the view
	 * is held and the hold waits for it. In a world frame outside a hold it is a tap error (Uncovered): nothing re-enters a hold for it.
	 */
	void NoteUndrawable(View a_view, std::string_view a_why);

	/** @brief Render thread, frame start. The world's first frame under a held menu starts the hold. */
	void OnFrameStart();
	/** @brief Render thread, frame end. Judges the hold and ends it once settled. */
	void OnFrameEnd();
}

#pragma once

// The hold after every load (dclf-architecture.md, "Load screens"): the loading screen stays up until DCLF draws the load's objects, so
// the engine draws none of them and nothing pops in.
// - The load coordinator (SE 13214 / AE 13363) ends a load by posting the loading menu's close (+0x211: UIMessageQueue::AddMessage of
//   "Loading Menu", kHide). The hold keeps that message. The menu stays open and keeps presenting, and it pauses the game. The world's
//   frames run under it, and DCLF builds its tables from them: the load is done, so FrameView::IsLoadingScreenUp is false for them.
// - A load with no loading menu (the coordinator showed none) holds the presents instead: the frame before the load stays on screen.
// - While it holds, the registration hooks withhold every pass of DCLF's classes from the views DCLF draws (PassCapture::HeldByLoad), and
//   the engine's culls of the views DCLF does not cover yet are skipped rather than handed over whole (the sun's cascades, the occlusion
//   maps: NoteHeld).
// - It ends after two settled frames in a row: the installed publication was committed two frames after the world's first one, built,
//   with no slot waiting on a transient reason (ScenePublication::TransientWaiting); the claims installed and unchanged since the last
//   frame; no draw family waiting for what it draws with (IndirectDraws::DrawFamiliesWaiting); no view held. The close is posted then (or
//   the presents resume). A hold that makes no progress for a few seconds, or lasts a minute, ends too, and logs what still waits. It never
//   hands a frame to the engine: what is not ready after it stays withheld until it is (SetSnapshot::pending).
//
// Install, the hooks: main thread. Active and MenuHeld: any thread. The rest: the render thread.

#include <cstdint>

namespace DCLF::LoadHold
{
	/**
	 * @brief Main thread, PostPostLoad: the load coordinator's hooks (its loading screen's start and its menu's close). A site another
	 * plugin changed is left alone and logged: loads are then not held.
	 */
	void Install();

	/** @brief Any thread: a hold is in effect (the world's frames run off the screen, DCLF's classes withheld from the engine). */
	bool Active();
	/** @brief Any thread: a hold keeps the loading menu open (from the world's first frame under it until the menu closes). */
	bool MenuHeld();
	/** @brief Render thread, at Present: a hold of a load with no loading menu keeps the previous frame on screen (the present skipped). */
	bool PresentsHeld();

	/** @brief The views a hold skips the engine's cull of. */
	enum class View : std::uint8_t
	{
		Sun,
		Occlusion
	};
	/** @brief Render thread: a view this frame was held (its engine cull skipped) because DCLF does not cover it yet. */
	void NoteHeld(View a_view);

	/** @brief Render thread, the frame's start (FrameDriver::BeginFrame): the world's first frame under a held menu starts the hold. */
	void OnFrameStart(bool a_running);
	/** @brief Render thread, the frame's end (FrameDriver::EndFrame, a_running: DCLF drew it): the hold judged, and ended once settled. */
	void OnFrameEnd(bool a_running);
}

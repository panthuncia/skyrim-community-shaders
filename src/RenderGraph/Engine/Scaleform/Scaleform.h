#pragma once

namespace Snapshot
{
	struct FrameSnapshot;
}

/**
 * @brief Scaleform on the render graph, in graph mode (Engine::GraphMode).
 *
 * The engine's GRendererD3D1x object gets a vtable copy whose display, target, draw, blend, mask and filter slots are CS's. On the
 * render thread with the graph up, each display is drawn by the Interface anchor's epoch with transcribed game shaders. A render
 * target GFx pushes inside a display is drawn into a scratch image. Draws the graph cannot take are left out and counted.
 *
 * Guest reads the D3D11 renderer's fields at each call; off the render thread or before setup, calls go to D3D11 unchanged.
 * Native mode takes every slot but the destructor: it records every call into the UI stream (UiStream.h), draws the stream's
 * replay, and keeps GFx's textures, render targets and video mode itself (UiResources.h). A setup failure is fatal.
 */
namespace RenderGraph::Engine::Scaleform
{
	/** @brief Reserves the Interface anchor and hooks the renderer's factory (SE 82831 / AE 84952). Must run before the graph initializes. */
	void Install();

	/**
	 * @brief Whether CS owns every slot of the game's renderer but its destructor (native mode, once the renderer exists). Then
	 * MenuManager::DrawInterface draws nothing itself: it is a game-side producer of the UI stream (decision 16). Any thread.
	 */
	bool OwnsRenderer();

	/**
	 * @brief Native mode, game thread, in a routed update before its snapshot is published (phase 7): MenuManager::DrawInterface's
	 * display loop (SE 79947 / AE 82084) without its render state (the target binds, ApplyState, the viewports, the closing ISCopy).
	 * Each shown menu's PostDisplay runs under the movie and menu stack locks; GFx's calls are only recorded, into
	 * Snapshot::Building()'s UI stream holder. The render side draws the stream with ReplaySnapshotUi, which applies the target rule
	 * the engine's binds made. Only for frames where GFx alone records inside DrawInterface (RecordsUiOnly).
	 */
	void ProduceUi(RE::UI* a_ui);

	/**
	 * @brief Game thread: whether this update's DrawInterface can be recorded alone (ProduceUi): an unpaused world frame without the
	 * map, by the snapshot and by the UI asked now (a menu the update's UI jobs opened draws in this DrawInterface already), and no
	 * offscreen PreDisplay due. A capture point.
	 */
	bool RecordsUiOnly(const Snapshot::FrameSnapshot& a_snapshot);

	/**
	 * @brief The UI point, native mode, render owner: DrawInterface's render-side work (Upscaling's PostDisplay, the clean-scene copy,
	 * HDR's UI buffer, temporal off), then a_snapshot's UI stream replayed whole from its header, then temporal back. A repeated
	 * render of the snapshot calls it again. Call it where Main::DrawFrame calls DrawInterface.
	 */
	void ReplaySnapshotUi(const Snapshot::FrameSnapshot& a_snapshot);

	/**
	 * @brief Whether DrawInterface's render-side work runs at CS's UI point (native, DrawInterface hooked). Then the features'
	 * DrawInterface detours only call through. Any thread.
	 */
	bool OwnsUiPoint();

	/**
	 * @brief Engine render work other than GFx inside DrawInterface (a menu scene, an ImageSpace effect, the local map). In a
	 * record-only DrawInterface it would draw out of order with the displays: counted and logged once. Any thread.
	 */
	void NoteOtherUiWork(const char* a_what);
}

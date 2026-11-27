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
	 * @brief Native mode: this thread produces the UI without render ownership (phase 7's game thread). Open it around a call of
	 * MenuManager::DrawInterface (SE 79947 / AE 82084) in a game update, before the snapshot is published. GFx's calls are only
	 * recorded, into Snapshot::Building()'s UI stream holder. Nothing is replayed and no render-owned state is touched: the render
	 * side draws the stream with ReplaySnapshotUi. Only for frames where GFx alone records inside DrawInterface (an unpaused world
	 * frame without the map, see snap7-thread.md "Package U"). Without the scope, the render owner's DrawInterface decides the mode
	 * itself.
	 */
	class ProducerScope
	{
	public:
		ProducerScope();
		~ProducerScope();
		ProducerScope(const ProducerScope&) = delete;
		ProducerScope& operator=(const ProducerScope&) = delete;
	};

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

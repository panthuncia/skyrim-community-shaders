#pragma once

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
}

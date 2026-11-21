#pragma once

/**
 * @brief Scaleform on the render graph, in graph mode (Engine::GraphMode).
 *
 * The engine's GRendererD3D1x keeps its bookkeeping, but its object gets a vtable copy whose display, target, draw, blend, mask
 * and filter slots are CS's. On the render thread with the graph up, each display is captured and drawn by the Interface
 * anchor's epoch with transcribed game shaders. A render target GFx pushes inside a display is drawn into a scratch image.
 * Draws the graph cannot take are left out and counted. Off the render thread or before setup, calls go to D3D11 unchanged. A setup failure is fatal.
 */
namespace RenderGraph::Engine::Scaleform
{
	/** @brief Reserves the Interface anchor and hooks the renderer's factory (SE 82831 / AE 84952). Must run before the graph initializes. */
	void Install();
}

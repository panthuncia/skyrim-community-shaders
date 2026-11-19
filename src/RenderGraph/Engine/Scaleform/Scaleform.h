#pragma once

/**
 * @brief Scaleform on the render graph, behind CS_ORG_SCALEFORM=1.
 *
 * The engine's GRendererD3D1x keeps its bookkeeping, but its object gets a vtable copy whose display, target, draw, blend, mask
 * and filter slots are CS's. On the render thread with the graph up, each display is captured and drawn by the Interface
 * anchor's epoch with transcribed game shaders. A render target GFx pushes inside a display is drawn into a scratch image.
 * Draws the graph cannot take are left out and counted. Off the render thread or with the path off, calls go to D3D11 unchanged.
 */
namespace RenderGraph::Engine::Scaleform
{
	bool Requested();

	/** @brief Reserves the Interface anchor and hooks the renderer's factory (SE 82831 / AE 84952). Must run before the graph initializes. */
	void Install();

	/** @brief Sets up the Interface client, its surfaces and pipelines. */
	void Setup();
}

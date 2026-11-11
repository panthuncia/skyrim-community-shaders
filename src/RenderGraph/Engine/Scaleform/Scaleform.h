#pragma once

/**
 * @brief Scaleform on the render graph (org-full-port.md P5.10, docs/development/p5/scaleform.md), behind CS_ORG_SCALEFORM=1.
 *
 * The engine's Scaleform renderer (GFx 3.3's GRendererD3D1x) stays the D3D11 renderer. Its object gets a copy of its vtable in
 * which the display, draw, style, blend and mask slots are CS's. On the render thread (FrameAnchors::OnRenderThread), with the
 * graph up, each display
 * (BeginDisplay to EndDisplay) is captured: its draws become ORG draws (Scaleform/FillVS.hlsl and FillPS.hlsl, the transcribed
 * game shaders), drawn by the Interface anchor's epoch at EndDisplay onto the target the display began on. A display that uses
 * what the graph path does not draw (filters, render targets of its own, 3D, line strips, video textures), or that the graph
 * cannot take, is replayed into the D3D11 renderer whole, at the same point. Off the render thread (the loading screen without
 * CS_ORG_LOADING_THREAD=1) and with the switch off, every call goes to the D3D11 renderer unchanged.
 */
namespace RenderGraph::Engine::Scaleform
{
	/** @brief Whether CS_ORG_SCALEFORM=1 asks for the ORG path. */
	bool Requested();

	/**
	 * @brief The seam (Hooks::Install, at PostPostLoad, before the render graph initializes): when Requested(), reserves the
	 * Interface anchor and hooks the renderer's factory (SE 82831 / AE 84952), which gives the renderer CS's vtable. Nothing
	 * otherwise.
	 */
	void Install();
}

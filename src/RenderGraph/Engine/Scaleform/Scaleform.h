#pragma once

/**
 * @brief Scaleform on the render graph (org-full-port.md P5.10, docs/development/p5/scaleform.md), behind CS_ORG_SCALEFORM=1.
 *
 * The engine's Scaleform renderer (GFx 3.3's GRendererD3D1x) keeps its bookkeeping. Its object gets a copy of its vtable in which
 * the display, render target, draw, blend, mask and filter slots are CS's. On the render thread (FrameAnchors::IsRenderOwner), with
 * the graph up, each display (BeginDisplay to EndDisplay) is captured: its draws become ORG draws (Scaleform/FillVS.hlsl and
 * FillPS.hlsl, the transcribed game shaders; the filters through FilterVS.hlsl and FilterPS.hlsl), drawn by the Interface anchor's
 * epoch onto the target the display began on. A render target GFx pushes inside a display is drawn into a scratch image of the
 * client's, as its own segment, and the draws that read it read that image (docs/development/parity/scaleform.md). A draw the
 * graph cannot take (a video texture, a lost one) is left out and counted. Off the render thread (the loading screen without
 * CS_ORG_LOADING_THREAD=1), with the graph path off, and with the switch off, every call goes to the D3D11 renderer unchanged.
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

	/**
	 * @brief Engine::PostSetup, when Requested(): the Interface client with its surfaces, and every pipeline queued
	 * (Passes::Setup). Nothing otherwise.
	 */
	void Setup();
}

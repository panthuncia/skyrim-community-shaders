#pragma once

#include <string>

/**
 * @brief The engine's ImageSpace draws on the render graph (org-full-port.md P5.6; design and RE in docs/development/p5/imagespace.md).
 *
 * With CS_ORG_IMAGESPACE=1 the ImageSpace effects this port covers are drawn by graph passes in the engine's place. The engine's CPU
 * side is unchanged: it decides which effects run, binds their targets and textures and writes their constants. At the engine's draw
 * (BSGraphics::Renderer::DrawTriShape inside BSImagespaceShader::Render, 100950 / 107732) the draw is withheld and latched instead:
 * the shadow state names the output and input render targets, the samplers and the blend state, and the engine-constants capture
 * (CS_ORG_ENGINE_CONSTANTS) holds the PerGeometry bytes. The latched draws run as a batch at the ImageSpace anchor, each through its
 * run gate, with CS's HLSL for the effect built for ORG (ISHDR, ISBlur, ISCompositeLensFlareVolumetricLighting).
 *
 * Covered: the HDR effect's own passes (the scene downsample, the luminance chain, the light adaptation, the bright-pass bloom blur)
 * and the volumetric lighting / lens flare composite of the pre-tonemap composites. The tonemap draw stays with D3D11 (Effects11, Post
 * Processing and HDR Display own it), as does every effect outside the list.
 *
 * Graph rebuilds: the client is registered once the shapes (shader, targets, samplers, blend) of a learning window are known; every
 * shape is a pass behind a gate entry, so effects that run on some frames only and the luminance chain's alternating targets never
 * rebuild the graph. A new shape later draws with D3D11 until the next registration, which happens only after a cell change or when
 * the engine's targets were made again (a resize).
 *
 * Fallback, per draw: anything the port cannot take (an effect not selected, a target outside the engine's table, a custom viewport,
 * a scissor rectangle, dynamic resolution, a clear, no captured constants, a call off the main thread, a graph not built yet, the
 * frame's anchor runs used up) is drawn by the engine as before. A batch that is pending when the engine draws something else is run
 * first, so the engine's order is kept. A failed graph returns every effect to D3D11 for the session.
 *
 * Switches: CS_ORG_IMAGESPACE=1 (default off: nothing is hooked), CS_ORG_IMAGESPACE_EFFECTS=<list> (engine names, effect names or
 * the groups hdr, bloom, composite; unset or "all": every covered effect), CS_ORG_IMAGESPACE_LOG=1 (each new shape and each batch).
 */
namespace RenderGraph::Engine::ImageSpace
{
	/** @brief Whether CS_ORG_IMAGESPACE=1 is set. Any thread. */
	bool Requested();

	/** @brief With the other engine hooks (Hooks.cpp): the render, scope and draw detours, when requested. */
	void Install();

	/** @brief State::Setup, before the render graph initializes: the ImageSpace anchor is reserved. */
	void Reserve();

	/** @brief State::Setup, once the graph is up: the latch and the run gate. The client registers after its learning window. */
	void Setup();

	/** @brief One status line (phase, passes, taken and refused draws). Main thread. */
	std::string Status();
}

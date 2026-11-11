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
 * Covered: the HDR effect's own passes (the scene downsample, the luminance chain, the light adaptation, the bright-pass bloom blur),
 * the volumetric lighting / lens flare composite of the pre-tonemap composites, RenderSSAO's chain (the camera depth, its mips, the
 * raw AO, the blurs and the composite, which reads IBL's and Exponential Height Fog's outputs by published id; drawn through the
 * explicit-target Render, 100951 / 107733), and the screen-space reflection pass (the mask downsample, the ray march, and its
 * separable compute blur), the end-of-frame chain's depth of field or distant blur (with its compute blur), refraction and radial
 * blur, the snow pass's ISCopySubRegionCS and Main::RenderWaterEffects' flow copies (each queued texture read by its heap index
 * from the latch). The tonemap draw stays with D3D11 (Effects11, Post Processing and HDR Display own it), as does every effect
 * outside the list (the volumetric lighting's apply and blurs: Volumetric Lighting's port).
 *
 * The compute form: inside a scope, BSImagespaceShader::DispatchComputeShader (100952 / 107734) names the effect, and its
 * DispatchCSShader (75532 / 77329) is withheld and latched: the shadow state's compute slots name the input and output targets
 * (CSTexture, CSUAV) and samplers, the engine's compute constants (CS b0) are captured, and the group counts open the pass's gate
 * entry (RunGate::OpenDispatch).
 *
 * Scopes: the HDR effect, the pre-tonemap composites, RenderSSAO and the reflection pass, and each call of an own-scope effect made
 * outside them (the end-of-frame effects, the depth-of-field blur, the snow copy, the water blend, the LDR downsample, each flow
 * copy). A batch runs at its scope's end at the latest (CS code between these calls uses D3D11 and runs other anchors). Each kind of
 * scope has its own share of the anchor's 19 runs a frame, so the early scopes never use up the later ones' runs.
 *
 * The pass set is fixed for the session at Setup (docs/development/p5/imagespace.md, "The primary path"): every shape (effect,
 * outputs, inputs, samplers, blend state, viewport, all by value) of the shape manifest beside the shader store, with the shapes
 * the RE says they imply, is a pass behind a gate entry, registered once. Programs are queued at Setup and taken by each pass when
 * the graph is first built (ProgramCache::GetNow), before any world frame. A shape outside the manifest stays with the engine for
 * the session (logged once) and is written to the manifest for the next one: nothing registers again during play. Two kinds of
 * shape need no session to see them first: the flow copy (its flow map is made at Setup if the engine has not made it) and the
 * whole-target shape of an effect that keeps the viewport before it (refraction, the distant blur). The engine's
 * targets made again (a resize) register the same shapes on the new imports, into the build the new imports ask for anyway.
 *
 * Inherent refusals, per draw: an effect not selected, a target outside the engine's table, a scissor rectangle, dynamic
 * resolution's quad, a manager rectangle, a clear at the bind, a shape outside the session's set, an anchor that cannot run another
 * batch now (FrameAnchors::CanRun, its runs a frame). Constants the capture missed are failures, logged as errors. A batch that is
 * pending when the engine draws something else is run first, so the engine's order is kept. A failed graph returns every effect to
 * the engine for the session. A covered effect's draw the engine still makes is not vouched for in the flush latch (no
 * Flush::Reason): under the latch it is skipped, and the residue census counts it.
 *
 * Switches: CS_ORG_IMAGESPACE=1 (default off: nothing is hooked). Diagnostics: CS_ORG_IMAGESPACE_EFFECTS=<list> (engine names,
 * effect names or the groups hdr, bloom, composite, sao, ssr; unset or "all": every covered effect) narrows the port to bisect an
 * effect; CS_ORG_IMAGESPACE_LOG=1 logs each shape new to the manifest and each effect's constant sizes.
 */
namespace RenderGraph::Engine::ImageSpace
{
	/** @brief Whether CS_ORG_IMAGESPACE=1 is set. Any thread. */
	bool Requested();

	/** @brief With the other engine hooks (Hooks.cpp): the render, scope and draw detours, when requested. */
	void Install();

	/** @brief State::Setup, before the render graph initializes: the ImageSpace anchor is reserved. */
	void Reserve();

	/** @brief State::Setup, once the graph is up: the latch, the run gate, the manifest's shapes registered and their programs queued. */
	void Setup();

	/** @brief One status line (passes, manifest, taken and refused draws). Main thread. */
	std::string Status();
}

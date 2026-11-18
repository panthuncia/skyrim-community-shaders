#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/**
 * @brief The engine's ImageSpace draws on the render graph (org-full-port.md P5.6; design and RE in docs/development/p5/imagespace.md).
 *
 * With CS_ORG_IMAGESPACE=1 the ImageSpace effects this port covers are drawn by graph passes in the engine's place. The engine's CPU
 * side is unchanged: it decides which effects run, binds their targets and textures and writes their constants. At the engine's draw
 * (BSGraphics::Renderer::DrawTriShape inside BSImagespaceShader::Render, 100950 / 107732) the draw is withheld: the engine-constants
 * capture (CS_ORG_ENGINE_CONSTANTS) holds its PerGeometry bytes, and the CS step that stands for it draws through its run gate, with
 * CS's HLSL for the effect built for ORG (ISHDR, ISBlur, ISCompositeLensFlareVolumetricLighting).
 *
 * Covered: the HDR effect's own passes (the scene downsample, the luminance chain, the light adaptation, the bright-pass bloom blur),
 * the volumetric lighting / lens flare composite of the pre-tonemap composites, RenderSSAO's chain (the camera depth, its mips, the
 * raw AO, the blurs and the composite, which reads IBL's and Exponential Height Fog's outputs by published id; drawn through the
 * explicit-target Render, 100951 / 107733), and the screen-space reflection pass (the mask downsample, the ray march, and its
 * separable compute blur), the end-of-frame chain's depth of field or distant blur (with its compute blur), refraction and radial
 * blur, the snow pass's ISCopySubRegionCS and Main::RenderWaterEffects' flow copies (each queued texture read by its heap index
 * from the latch). The tonemap draw is taken too (5.5.4: ISHDR's BLEND build, the plain and the fade form) when the engine makes it,
 * that is when Effects11 does not replace the HDR effect. Also the copies (ISCopyScaleBias and ISCopyTextureMask through ISCopy.hlsl's
 * ORG vertex stage; a copy from kFRAMEBUFFER reads the back buffer's import), the vanilla TAA (two targets), the SIL composite, the
 * snow's SSS, the local map's edge pass and the colour fill; since round 2 the race menu's face tint (ISAlphaBlend, its loaded
 * tint masks by heap index from the latch: EffectInfo::latched), the reflections' apply, the lighting composite, the dynamic-resolution
 * upsample, the parallax-mask blur, the IB lens flares, the SIL raw indirect lighting, FXAA and the underwater mask. ISMap and both
 * ISNoise forms rasterize nothing (their vertex shaders
 * write no SV_Position): they are taken with no pass. The volumetric lighting's apply and blurs are Volumetric Lighting's port.
 *
 * The compute form: inside a scope, BSImagespaceShader::DispatchComputeShader (100952 / 107734) names the effect, and its
 * DispatchCSShader (75532 / 77329) is withheld: the engine's compute constants (CS b0) are captured, and the group counts open the
 * step's gate entry (RunGate::OpenDispatch).
 *
 * Scopes: the HDR effect, the pre-tonemap composites, RenderSSAO and the reflection pass, and each call of an own-scope effect made
 * outside them (the end-of-frame effects, the depth-of-field blur, the snow copy, the water blend, the LDR downsample, each flow
 * copy). A batch runs at its scope's end at the latest (CS code between these calls uses D3D11 and runs other anchors). The anchor
 * makes as many runs a frame as the batches need (run leases): no scope runs out of them.
 *
 * The passes are CS's (docs/development/org-native-frame-graph.md, "The post stage"), in both graph modes: CS declares each engine
 * effect step as a pass of its own (WorldSteps.inl, ChainSteps.inl, StartSteps.inl, MenuSteps.inl), held by a CS stage. The engine's
 * draw is withheld and its hook only stages the constants the engine computed for that step. A draw CS has no pass for is a mismatch
 * (Frame::Driver::NoteTapError, NoPass). Setup turns the port off when a stage with ImageSpace steps does not run in the mode.
 *
 * Switches: CS_ORG_IMAGESPACE=1 (default off: nothing is hooked). Diagnostics: CS_ORG_IMAGESPACE_EFFECTS=<list> (engine names,
 * effect names or the groups hdr, bloom, composite, sao, ssr; unset or "all": every covered effect) narrows the port to bisect an
 * effect.
 */
namespace RenderGraph::Engine
{
	struct TonemapTargets;
}

namespace RenderGraph::Engine::ImageSpace
{
	/**
	 * @brief The water's flow map's published id: the flow copies' import of it (FrameAnchors::D3D11Texture "engine.flow-map"), published
	 * at Setup (PublishedResources::PublishImport) so the water's draws declare the texture the copies write and read it by id. Not
	 * published without the port or without the flow copy among its effects.
	 */
	inline constexpr std::string_view kFlowMapId = "cs.d3d11.engine.flow-map";

	/** @brief Whether CS_ORG_IMAGESPACE=1 is set. Any thread. */
	bool Requested();

	/** @brief With the other engine hooks (Hooks.cpp): the render, scope and draw detours, when requested. */
	void Install();

	/** @brief State::Setup, before the render graph initializes: the ImageSpace anchor is reserved. */
	void Reserve();

	/** @brief State::Setup, once the graph is up: the step clients' run gates and latches, and the menus' composites registered. */
	void Setup();

	/**
	 * @brief RenderEndOfFrameEffects (99023 / 105674) of the world chain: a_begin at its entry, with its source and final targets
	 * (its arguments), where CS checks the engine's loop against the frame's description (the chain's targets are fixed); false at its
	 * return. Render owner.
	 */
	void OnEndOfFrame(bool a_begin, std::int32_t a_source, std::int32_t a_final);

	/**
	 * @brief RenderEndOfFrameEffects outside the world chain (a menu scene's, from UI3DSceneManager::Render): a_begin at its entry with its
	 * source and final targets, false at its return. Its HDR effect's draws are the menu composites' steps. Render owner.
	 */
	void OnMenuEndOfFrame(bool a_begin, std::int32_t a_source, std::int32_t a_final);

	/** @brief Renderer::Begin, after the frame's schedule was chosen (Frame::Driver): the menu composites' registration is checked. Render owner. */
	void OnFrameBegin();

	/**
	 * @brief The published id of the CS target that stands for the engine's adapted luminance target a_view is a view of (the post chain
	 * draws the luminance chain's last level of each parity into a CS pair, "ImageSpace::AdaptedLuminance0/1"), or empty when CS does
	 * not own the pair or a_view is another target. Drawcall Limit Fix's character light reads CS's target through it. Render owner.
	 */
	std::string_view AdaptedLuminanceOf(const ID3D11ShaderResourceView* a_view);

	/** @brief The adapted luminance pair's published ids (AdaptedLuminanceOf), for a reader's declarations. */
	std::span<const std::string_view> AdaptedLuminanceIds();

	/**
	 * @brief The Tonemap anchor's targets while the world post chain's HDR effect runs in native mode: CS's fixed images, kMAIN before
	 * the tonemap and the final target after it. False elsewhere (guest, a menu scene's tonemap): the engine's call names them. Render
	 * owner.
	 */
	bool ChainTonemapTargets(TonemapTargets& a_out);

	/** @brief One status line (taken and refused draws). Main thread. */
	std::string Status();
}

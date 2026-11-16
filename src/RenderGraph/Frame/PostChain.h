#pragma once

#include "RenderGraph/Frame/FrameDescription.h"

#include <cstdint>

/**
 * @brief The world post chain's engine state, read by CS (docs/development/org-native-frame-graph.md, "The post stage").
 *
 * The chain is ImageSpaceManager::RenderWorldPostProcess (SE 100430, AE 107148): the volumetric lighting, then
 * RenderPreTonemapComposites (99024 / 105675), then RenderEndOfFrameEffects (99023 / 105674: the end-of-frame slots, HDR or a copy,
 * TAA, the screenshot copy, the LDR slots, the dynamic-resolution upsample). CS draws it with passes of its own (ImageSpace's
 * ChainSteps.inl). This file reads the engine state those passes need:
 * - at Renderer::Begin, the chain's structure (PostFields): which effects the post stage has on for the frame;
 * - the RenderWorld scopes' state (WorldFields) for the deferred+water stage's description;
 * - the chain's two entries (detoured): RenderWorldPostProcess, which brackets the world chain, and RenderEndOfFrameEffects, where
 *   ImageSpace plans the chain's targets and whose return ends the post stage.
 */
namespace RenderGraph::Frame::PostChain
{
	/** @brief Installs the chain's detours (RenderWorldPostProcess, RenderEndOfFrameEffects). With the ImageSpace hooks, native mode. */
	void Install();

	/** @brief Whether the detours are installed (CS_ORG_SEGMENT_POST not 0). Any thread. */
	bool On();

	/** @brief Reads the post fields from the engine now. Render owner. read is false when the ImageSpace manager does not exist yet. */
	PostFields Read();

	/**
	 * @brief The RenderWorld scopes (Main::RenderWorld, SE 100424 / AE 107142), in the order it runs them. CS draws them as passes of
	 * the deferred+water stage (ImageSpace's world steps); ReadWorld reads the engine state that selects their variants.
	 */
	enum class Scope : std::uint8_t
	{
		Reflections,    // the screen-space reflections (100428 / 107146)
		Ssao,           // RenderSSAO's first call: the camera depth, its levels, the raw AO and its blur
		Snow,           // the snow pass (RenderSnowSSS, 100537 / 107267) or the copy of kMAIN into its copy target
		SsaoComposite,  // RenderSSAO's second call: the composite into kMAIN
		WaterBlend,     // the water's temporal blend (ISWaterBlend)
		Count,
		None = Count
	};

	/** @brief "reflections". */
	const char* ScopeName(Scope a_scope);

	/** @brief Reads the world fields a_scope selects from the engine now (its entry). Render owner. */
	WorldFields ReadWorld(Scope a_scope, const WorldFields& a_base);

	/** @brief Whether the world post chain is running now (RenderWorldPostProcess's entry to its return). Render owner. */
	bool InChain();

	/**
	 * @brief The luminance chain's level count for kHDR_DOWNSAMPLE0's size now, as ImageSpaceEffectHDR::Render (100465 / 107183)
	 * loops: a quarter per level down to 1x1, or 2x2 with the option (513488 / 391336) and without its override (528106 / 415051).
	 */
	std::uint32_t LuminanceLevels();

	/**
	 * @brief The luminance chain's last-level parity global (528109 / 415054). ImageSpaceEffectHDR::Render flips it just before it
	 * draws the last level, which goes to target 27 + levels - 1 + 2 * parity; read at a draw after that, it is that draw's parity.
	 */
	std::uint32_t LuminanceParity();

	/**
	 * @brief Which of its two targets the volumetric lighting writes this frame and the composite reads: kIMAGESPACE_VOLUMETRIC_LIGHTING
	 * + (~counter & 1), the counter at +0x88 of the VL state (getter 100297 / 107014). 0 when the state does not exist.
	 */
	std::uint32_t VolumetricParity();

	/** @brief Renderer::Begin (FrameDriver::OnBegin): fills a_frame.post and counts how often the structure changes. Render owner. */
	void OnBegin(Description& a_frame);

	/** @brief The census line (CS_ORG_EPOCH_STATS's report): world frames, and the structure's changes by field. */
	void LogStats();
}

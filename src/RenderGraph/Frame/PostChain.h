#pragma once

#include "RenderGraph/Frame/FrameDescription.h"

#include <cstdint>

/**
 * @brief Reads the world post chain's engine state for CS's own post passes. See org-native-frame-graph.md, "The post stage".
 *
 * The chain is ImageSpaceManager::RenderWorldPostProcess (SE 100430, AE 107148), which runs the volumetric lighting,
 * RenderPreTonemapComposites (99024 / 105675) and RenderEndOfFrameEffects (99023 / 105674).
 * This file reads which effects are on at Renderer::Begin, the RenderWorld scopes' state, and detours the chain's two entries.
 */
namespace RenderGraph::Frame::PostChain
{
	/** @brief Installs the chain's detours. Native mode, with the ImageSpace hooks. */
	void Install();

	/** @brief Whether the detours are installed (CS_ORG_SEGMENT_POST not 0). Any thread. */
	bool On();

	/** @brief Reads the post fields from the engine now. Render owner. read is false before the ImageSpace manager exists. */
	PostFields Read();

	/** @brief The RenderWorld scopes (Main::RenderWorld, SE 100424 / AE 107142), in the order it runs them. */
	enum class Scope : std::uint8_t
	{
		Reflections,    // 100428 / 107146
		Ssao,           // RenderSSAO's first call, up to the raw AO and its blur
		Snow,           // RenderSnowSSS (100537 / 107267), or the copy of kMAIN
		SsaoComposite,  // RenderSSAO's second call, the composite into kMAIN
		WaterBlend,     // ISWaterBlend
		Count,
		None = Count
	};

	/** @brief Reads the fields a_scope selects from the engine at its entry. Render owner. */
	WorldFields ReadWorld(Scope a_scope, const WorldFields& a_base);

	/** @brief Whether the world post chain is running now. Render owner. */
	bool InChain();

	/**
	 * @brief The luminance chain's level count, as ImageSpaceEffectHDR::Render (100465 / 107183) loops. It goes down a quarter
	 * per level to 1x1, or to 2x2 with the option (513488 / 391336) and without its override (528106 / 415051).
	 */
	std::uint32_t LuminanceLevels();

	/**
	 * @brief The luminance chain's last-level parity (528109 / 415054). The engine flips it just before drawing the last level,
	 * which goes to target 27 + levels - 1 + 2 * parity.
	 */
	std::uint32_t LuminanceParity();

	/** @brief The VL target this frame is kIMAGESPACE_VOLUMETRIC_LIGHTING + (~counter & 1). 0 when the VL state does not exist. */
	std::uint32_t VolumetricParity();

	/** @brief Fills a_frame.post at Renderer::Begin and counts structure changes. Render owner. */
	void OnBegin(Description& a_frame);

	/** @brief The census line for CS_ORG_EPOCH_STATS. */
	void LogStats();
}

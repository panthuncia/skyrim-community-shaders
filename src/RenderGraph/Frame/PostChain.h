#pragma once

#include "RenderGraph/Frame/FrameDescription.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief The post chain's description: which ImageSpace effects the engine's world post chain runs this frame, read by CS from the
 * engine's own state (docs/development/org-native-frame-graph.md, "The declared frame, stage 4").
 *
 * The chain is ImageSpaceManager::RenderWorldPostProcess (SE 100430, AE 107148): the volumetric lighting, then
 * RenderPreTonemapComposites (99024 / 105675: SIL, the image-based lens flares, the VL and lens flare composite), then
 * RenderEndOfFrameEffects (99023 / 105674: the end-of-frame slots, HDR or a copy, TAA, the LDR slots, the dynamic-resolution upsample).
 * Its order is fixed in the engine's code. What varies is which effects run and which variant each takes, and every one of those
 * decisions reads state CS can read too (PostFields).
 *
 * - **Read.** At Renderer::Begin (OnBegin fills Description::post) and again at the chain's decision event: the entry of
 *   RenderWorldPostProcess, after the game's update and the menus have set the ImageSpace data for the frame. The second read is
 *   the one the chain is predicted from; a field that differs from Begin's is counted by name (the engine decided it after Begin).
 * - **Predict.** PredictEffects turns the fields into the sequence of ImageSpace shaders the chain draws or dispatches, by their index
 *   in ImageSpaceManager::effects, reading the composites' children where the engine renders a list (the HDR effect's children, the
 *   bloom blur's).
 * - **Check.** The ImageSpace hooks note every shader the chain draws (NoteShader). At the chain's end the sequence is compared with
 *   the prediction. A difference is a tap error ("post not declared"): the description missed something the engine did.
 *
 * The check needs the ImageSpace hooks (CS_ORG_IMAGESPACE, which native mode turns on). CS_ORG_SEGMENT_POST=0 turns it off.
 */
namespace RenderGraph::Frame::PostChain
{
	/** @brief Installs the decision event's detour (RenderWorldPostProcess). With the ImageSpace hooks, native mode. */
	void Install();

	/** @brief Whether the check runs (installed, CS_ORG_SEGMENT_POST not 0). Any thread. */
	bool On();

	/** @brief Reads the post fields from the engine now. Render owner. read is false when the ImageSpace manager does not exist yet. */
	PostFields Read();

	/** @brief The two parts of the world post chain, each predicted from the fields read at its own decision event. */
	enum class Part : std::uint8_t
	{
		Composites,  // RenderWorldPostProcess up to RenderEndOfFrameEffects: the volumetric lighting and the pre-tonemap composites
		EndOfFrame,  // RenderEndOfFrameEffects (99023 / 105674)
		All,
	};

	/**
	 * @brief The ImageSpace shaders the world post chain (or a_part of it) draws for a_fields, in order, by their index in
	 * ImageSpaceManager::effects. An entry at or above kGroupBase is a group (kGroupVolumetricLighting and the others below): draws of
	 * the group's shaders whose order and count the description does not settle yet (one or more; the volumetric lighting's zero or
	 * more, since Volumetric Lighting's port takes its draws before the hooks see them).
	 */
	std::vector<std::uint16_t> PredictEffects(const PostFields& a_fields, Part a_part = Part::All);

	inline constexpr std::uint16_t kGroupBase = 0x8000;
	inline constexpr std::uint16_t kGroupVolumetricLighting = kGroupBase + 0;  // the engine's VL apply and blur (Volumetric Lighting's port)
	inline constexpr std::uint16_t kGroupLensFlares = kGroupBase + 1;          // the image-based lens flares (+0x180)
	inline constexpr std::uint16_t kGroupDepthOfField = kGroupBase + 2;        // slot 3
	inline constexpr std::uint16_t kGroupFullScreenBlur = kGroupBase + 3;      // slot 6
	inline constexpr std::uint16_t kGroupGetHit = kGroupBase + 4;              // slot 7
	inline constexpr std::uint16_t kGroupWorldMap = kGroupBase + 5;            // slot 0
	inline constexpr std::uint16_t kGroupRadialBlur = kGroupBase + 6;          // slot 5
	inline constexpr std::uint16_t kGroupRefraction = kGroupBase + 7;          // slot 1

	/** @brief "ISHDRDownSample4" for an effect index, a group's name for a group. */
	std::string EffectName(std::uint16_t a_effect);

	/** @brief Renderer::Begin (FrameDriver::OnBegin): fills a_frame.post and counts how often the description changes. Render owner. */
	void OnBegin(Description& a_frame);

	/**
	 * @brief The ImageSpace hooks: the engine is about to draw or dispatch an ImageSpace shader. a_effect is the shader's ImageSpaceEffect
	 * (BSImagespaceShader::Render's this) or the BSImagespaceShader itself (the explicit Render and DispatchComputeShader). Counted only
	 * inside the world post chain. Render owner.
	 */
	void NoteShader(const void* a_effect);

	/** @brief The census line (CS_ORG_EPOCH_STATS's report): chains checked and matched, and the description's changes by field. */
	void LogStats();
}

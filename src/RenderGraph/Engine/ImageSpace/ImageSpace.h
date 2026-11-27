#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

/**
* @brief The engine's ImageSpace draws on the render graph, in graph mode (Engine::GraphMode).
*
* The engine's CPU side is unchanged. It picks the effects, binds targets and writes constants. At the engine's draw the draw is
* withheld, the constants capture holds its bytes, and a CS pass that stands for the step draws through its run gate with CS's
* HLSL for the effect. Dispatches work the same way at DispatchCSShader.
*
* Each engine effect step is a CS pass held by a CS stage (WorldSteps.inl, ChainSteps.inl, StartSteps.inl, MenuSteps.inl).
* A draw with no pass is a mismatch. Effects whose vertex shader writes no SV_Position are taken with no pass.
*/
struct ID3D11ShaderResourceView;

namespace RE
{
}

namespace RenderGraph::Engine
{
	struct TonemapTargets;
}

namespace RenderGraph::Engine::Native::WaterEffects
{
	struct Work;
}

namespace RenderGraph::Engine::ImageSpace
{
	struct GameInputs;
	struct WorldSettings;

	/**
	* @brief Native mode: the adopted snapshot's ImageSpace inputs, which the native post chain draws from, so the frame's description
	* at Begin reads the same values. Null in guest mode and before the first snapshot. Render owner.
	*/
	const GameInputs* SnapshotInputs();

	/**
	* @brief Native mode: the adopted snapshot's world settings (the RenderWorld scopes' INI values, switches and portal fog), which
	* the native scopes draw from, so the frame's description at Begin reads the same values. Null as SnapshotInputs. Render owner.
	*/
	const WorldSettings* SnapshotWorld();

	/**
	* @brief Native mode: whether this render frame applies a TAA history reset the game asked for (the loading menu's flag, taken in
	* the snapshot capture). True for one render, the first world render after the snapshot's count moves. False in guest mode.
	* Render owner.
	*/
	bool TaaResetDue();

	/** @brief Published at Setup so the water draws read the texture the flow copies write. Not published without the flow copy port. */
	inline constexpr std::string_view kFlowMapId = "cs.d3d11.engine.flow-map";

	/** @brief Call with the other engine hooks. Installs nothing unless requested. */
	void Install();

	/** @brief State::Setup, once the graph is up. */
	void Setup();

	/** @brief The world chain's RenderEndOfFrameEffects. True at entry with its arguments, false at return. Render owner. */
	void OnEndOfFrame(bool a_begin, std::int32_t a_source, std::int32_t a_final);

	/** @brief RenderEndOfFrameEffects for a menu scene. True at entry with its arguments, false at return. Render owner. */
	void OnMenuEndOfFrame(bool a_begin, std::int32_t a_source, std::int32_t a_final);

	/**
	* @brief Native: a menu scene's HDR effect from a_source into a_final, as UI3DSceneManager::Render's RenderEndOfFrameEffects draws it
	* for a light scheme with an ImageSpace override (manager +0xB0): the menu composite's HDR steps with CS's rows from the override's
	* data, and no engine call. False without an override or with CS's ImageSpace off. Render owner, inside the UI stage.
	*/
	bool MenuSceneHdr(std::int32_t a_source, std::int32_t a_final);

	/**
	* @brief RenderWorldPostProcess's hook (native-post-chain.md, family 4): reads the snapshot's B fields, then runs CS's native body
	* or a_engine, the engine's. Render owner.
	*/
	void WorldPostProcess(std::uint32_t a_target, const std::function<void()>& a_engine);

	/**
	* @brief Native: Main::RenderWorld's ImageSpace calls in its order (SE 100424, AE 107142): RenderSSAO, the snow pass or the copy of
	* kMAIN, then RenderSSAO's composite. CS's native RenderWorld calls it; the RenderSSAO calls take CS's route, not the engine's
	* call sites. Render owner.
	*/
	void RenderWorldImageSpace();

	/**
	* @brief Native: the pre-resolve's reflections pass (SE 100428, AE 107146) in native's RenderWorld, CS's steps with no engine call.
	* Nothing is drawn while CS cannot stage the world steps. Render owner.
	*/
	void NativeReflections();

	/** @brief Native: the post-resolve's water blend (SE 100550, AE 107285) in native's RenderWorld, as NativeReflections. Render owner. */
	void NativeWaterBlend();

	/** @brief A face tint bake's ISAlphaBlend pixel rows (PS b2), as the generic SetupGeometry would write them from its param. */
	struct TintRows
	{
		std::array<std::byte, 1024> bytes{};
		std::uint32_t size = 0;
		std::array<std::uint8_t, 64> offsets{};
		std::uint32_t count = 0;
	};

	/**
	* @brief Native, game thread (the capture): a_out from a face tint job's ImageSpaceShaderParam. Returns why it cannot, or null.
	* Reads the engine's ISAlphaBlend shader object for its row table.
	*/
	const char* BuildTintRows(const void* a_param, TintRows& a_out);

	/**
	* @brief Native: stages one face tint bake as the face tint step (ISAlphaBlend into kPLAYER_FACEGEN_TINT), from the bake's sixteen
	* mask views (null reads the null texture) and rows, with no engine call. False when the frame holds no build with the step yet.
	* Render owner, inside the start stage.
	*/
	bool StageFaceTint(const std::array<ID3D11ShaderResourceView*, 16>& a_masks, const TintRows& a_rows);

	/**
	* @brief Native: the water work the game side planned (Native/WaterEffects.h) as start steps with CS's rows: the displacement steps in
	* order, then the flow copies. Nothing is drawn while CS cannot stage the start steps. Render owner.
	*/
	void StageNativeWater(const Native::WaterEffects::Work& a_work);

	/**
	* @brief The world chain's RenderEndOfFrameEffects, between OnEndOfFrame's begin and end (family 2): CS's native end-of-frame effects,
	* or a_engine for a frame CS does not draw. Render owner.
	*/
	void WorldEndOfFrame(std::int32_t a_source, std::int32_t a_final, const std::function<void()>& a_engine);

	/** @brief Renderer::Begin, after the frame's schedule was chosen. Render owner. */
	void OnFrameBegin();

	/**
	* @brief The published id of the CS target that replaces the engine's adapted luminance target behind a_view.
	* Empty when CS does not own the pair. DCLF's character light reads through it. Render owner.
	*/
	std::string_view AdaptedLuminanceOf(const ID3D11ShaderResourceView* a_view);

	std::span<const std::string_view> AdaptedLuminanceIds();

	/** @brief Native mode, world chain HDR effect only: kMAIN before the tonemap and the final target after. False elsewhere. Render owner. */
	bool ChainTonemapTargets(TonemapTargets& a_out);

	/**
	* @brief Whether CS's steps draw the world chain's ImageSpace effects now, inside its RenderEndOfFrameEffects.
	* Then the scene the HDR tonemap reads is in kMAIN, whatever target the engine names. CS's refraction step copies back into kMAIN,
	* where the engine's refraction wrote kMAIN_COPY. Render owner.
	*/
	bool SceneInMain();
}

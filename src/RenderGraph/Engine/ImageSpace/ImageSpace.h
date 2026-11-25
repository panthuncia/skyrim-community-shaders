#pragma once

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
namespace RenderGraph::Engine
{
	struct TonemapTargets;
}

namespace RenderGraph::Engine::ImageSpace
{
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
	* @brief RenderWorldPostProcess's hook (native-post-chain.md, family 4): reads the snapshot's B fields, then runs CS's native body
	* or a_engine, the engine's. Render owner.
	*/
	void WorldPostProcess(std::uint32_t a_target, const std::function<void()>& a_engine);

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

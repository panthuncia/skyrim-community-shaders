#pragma once

#include <cstdint>

struct ID3D11BlendState;
struct ID3D11RasterizerState;

namespace DCLF
{
	/**
	 * @brief The engine's fixed-function state tables and the globals that pick a decal's depth bias.
	 * Community Shaders swaps the blend table for deferred variants between StartDeferred and ResetBlendStates, so a blend state
	 * read from it depends on when it is read.
	 */
	using RasterStateArray = ID3D11RasterizerState* [2][3][12][2];  // [fill][cull][depth bias][scissor]
	using BlendStateArray = ID3D11BlendState* [7][2][13][2];        // [blend mode][alpha to coverage][write mode][extra]

	RasterStateArray& EngineRasterStates();
	BlendStateArray& EngineBlendStates();
	/** @brief The G-buffer pass's blend state at these indices, or null until Deferred has made them. */
	ID3D11BlendState* DeferredBlendState(std::uint32_t a_blendMode, std::uint32_t a_alphaToCoverage, std::uint32_t a_writeMode, std::uint32_t a_extra);

	/**
	 * @brief The depth-bias mode the engine's decal groups draw with (FUN_1414b3bb0, FUN_1414b3d50, checked against real draws).
	 * Opaque decals use 6 + disableSunShadows while ToggleDepthBias is set. Blended decals always use 10 + disableSunShadows.
	 */
	std::uint32_t DecalDepthBiasMode(std::uint32_t a_decalGroup);
}

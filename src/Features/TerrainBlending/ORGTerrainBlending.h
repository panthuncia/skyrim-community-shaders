#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

namespace org
{
	class Resource;
}

/**
 * @brief Terrain Blending's depth blend on the render graph (CS_ORG_TERRAIN_BLENDING=1).
 *
 * What stays on D3D11, and why:
 * - The terrain depth: the z-prepass draws near terrain blocks into it a second time, without the depth offset. Drawcall Limit Fix
 *   draws them there itself when it holds the terrain (Integrations/TerrainBlending's near twin, into its own import of the
 *   texture); otherwise the engine's draws do, re-issued by Terrain Blending's hook. The engine's draws need a D3D11 depth-stencil
 *   view, which graph textures do not export, so the texture and its clear stay D3D11's.
 *
 * What runs on the graph: one compute pass at the DepthDone anchor, right after Main::RenderDepth returns. DepthBlend reads the
 * main depth (FrameAnchors::EngineDepth) and the terrain depth (FrameAnchors::ImportDepth) and writes three graph textures:
 * - "TerrainBlending::BlendedDepth" (R32_FLOAT) and "TerrainBlending::BlendedDepth16" (R16_UNORM): min(main, terrain). The engine
 *   and every D3D11 feature read the first through the depth table (Terrain Blending points kMAIN's and kPOST_ZPREPASS_COPY's depth
 *   SRV at its export until its terrain passes), so both export an SRV. Graph readers (Screen Space Shadows, Exponential Height Fog)
 *   read them by name.
 * - "TerrainBlending::Mask" (R32_FLOAT): the main depth, which the terrain draws read at t55. The D3D11 path copies the main depth
 *   into the terrain depth for this. It exports an SRV for the engine's terrain draws, and Drawcall Limit Fix's colour passes read it
 *   by name (Integrations/TerrainBlending's frame texture).
 * All three are published (PublishedResources), so passes of other epochs are ordered after the blend.
 *
 * Setup (TerrainBlending::SetupResources) makes the program and the textures and publishes them. The first depth pass imports the
 * depths and registers the client; a resize or a new import registers it again, since the pass holds what it was made with.
 *
 * On any failure the path stops for good: Terrain Blending makes its D3D11 textures and blends on D3D11 again.
 *
 * Everything here runs on the render thread. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGTerrainBlending
{
public:
	/** @brief The published ids (PublishedResources), the twins' names. */
	static constexpr std::string_view kBlendedDepthId = "TerrainBlending::BlendedDepth";
	static constexpr std::string_view kBlendedDepth16Id = "TerrainBlending::BlendedDepth16";
	static constexpr std::string_view kMaskId = "TerrainBlending::Mask";

	/** @brief Whether CS_ORG_TERRAIN_BLENDING=1 is set. */
	static bool Requested();

	/** @brief Reserves the DepthDone anchor when requested. Call before the render graph initializes (PostPostLoad). */
	static void Reserve();

	/** @brief The one instance. */
	static ORGTerrainBlending& Get();

	/**
	 * @brief After the render graph initializes (TerrainBlending::SetupResources), when requested: makes the program and the
	 * textures and publishes them. The device is the graph host's.
	 * @param a_onFailed Called whenever the path stops (here or later): Terrain Blending's D3D11 textures come back.
	 * @return Whether the blend is on the render graph; false keeps the D3D11 path.
	 */
	bool Setup(std::function<void()> a_onFailed = {});

	/**
	 * @brief At Terrain Blending's depth hook, before it points the depth table at the blended depth: imports the depths, follows
	 * the main target's size, and registers the client when either changed.
	 * @return Whether this frame blends on the graph; false: the D3D11 path.
	 */
	bool BeforeDepth();

	/**
	 * @brief After the z-prepass: runs the DepthDone anchor, whose pass blends the depths.
	 * @return Whether it ran; false: blend on D3D11 (the path may have failed and returned to D3D11 inside).
	 */
	bool Blend();

	/** @brief Whether the textures are the graph's and the runtime is still up. */
	bool Active() const;

	/**
	 * @brief TerrainBlending::ClearShaderCache: when the path stopped because DepthBlend.hlsl did not compile, sets it up again.
	 * Programs that built reload through the pipeline registry.
	 */
	void ClearShaderCache();

	/** @brief The blended depth's exported view (a_16bit: the R16 one); null when inactive. */
	ID3D11ShaderResourceView* BlendedDepthSRV(bool a_16bit) const;

	/** @brief The mask's exported view (t55); null when inactive. */
	ID3D11ShaderResourceView* MaskSRV() const;

	/** @brief The R32 blended depth's graph resource, published as kBlendedDepthId; null when inactive. */
	std::shared_ptr<org::Resource> BlendedDepthGraph() const;

	~ORGTerrainBlending();

	/** @brief The textures, imports and program the pass and the client share (ORGTerrainBlending.cpp). */
	struct Impl;

private:
	ORGTerrainBlending() = default;

	/** @brief Registers (or replaces) the client with the textures and imports as they are now. False, logged, on failure. */
	bool RegisterClient();

	/** @brief Stops the ORG path for good: Terrain Blending returns to its D3D11 textures. */
	void Fail(const char* a_why);

	std::shared_ptr<Impl> impl;
	std::function<void()> onFailed;
	bool failed = false;
	bool programFailed = false;  // the failure was the program's compile, which a shader reload may fix
};

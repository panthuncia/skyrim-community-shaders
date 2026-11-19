#pragma once

#include <d3d11.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace org
{
	class ExternalTextureResource;
	class Resource;
}

/**
* @brief Terrain Blending's depth blend on the render graph, with CS_ORG_TERRAIN_BLENDING=1.
*
* The terrain depth is a persistent, published depth twin exported to D3D11. Near terrain is drawn into it a second time
* without the depth offset, by DCLF or by the engine through the export. Its clear runs at EarlyPrepass.
* One compute pass at DepthDone writes min(main, terrain) as R32 and R16 blended depths, and copies the main depth into the mask.
* All three are exported, because the engine reads the blended depth through the depth table, and published for graph readers.
* On any failure the path stops for good and Terrain Blending blends on D3D11. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGTerrainBlending
{
public:
	/** @brief The published ids. */
	static constexpr std::string_view kBlendedDepthId = "TerrainBlending::BlendedDepth";
	static constexpr std::string_view kBlendedDepth16Id = "TerrainBlending::BlendedDepth16";
	static constexpr std::string_view kMaskId = "TerrainBlending::Mask";
	static constexpr std::string_view kTerrainDepthId = "TerrainBlending::TerrainDepth";

	static bool Requested();

	/** @brief Call in PostPostLoad, before the render graph initializes. */
	static void Reserve();

	static ORGTerrainBlending& Get();

	/**
	* @brief Call at SetupResources when requested.
	* @param a_onFailed Called whenever the path stops, now or later. Terrain Blending's D3D11 textures come back.
	* @return False keeps the D3D11 path.
	*/
	bool Setup(std::function<void()> a_onFailed = {});
	/**
	* @brief Call before Setup with the runtime active. Fills a_target with the twin's exports, one reference each as D3D11's Create gives.
	* The twin lives as long as the game. @return False means make a D3D11 texture.
	*/
	bool MakeTerrainDepth(RE::BSGraphics::DepthStencilData& a_target, const D3D11_TEXTURE2D_DESC& a_desc, DXGI_FORMAT a_srvFormat, DXGI_FORMAT a_dsvFormat);
	/** @brief Null when Terrain Blending made a D3D11 one. */
	std::shared_ptr<org::ExternalTextureResource> TerrainDepthGraph() const;
	/** @brief Whether this frame's EarlyPrepass run already cleared it, so the D3D11 clear can be skipped. */
	bool TerrainDepthCleared() const;

	/**
	* @brief Call before the depth hook points the depth table at the blended depth. Re-registers after a resize or new import.
	* @return False means the D3D11 path blends this frame.
	*/
	bool BeforeDepth();

	/** @brief After the z-prepass. @return False means blend on D3D11. */
	bool Blend();

	bool Active() const;

	/** @brief Retries setup after a failed DepthBlend.hlsl compile. Built programs reload through the pipeline registry. */
	void ClearShaderCache();

	/** @brief Null when the graph does not hold this frame's blend. */
	ID3D11ShaderResourceView* BlendedDepthSRV(bool a_16bit) const;

	/** @brief t55. Null when the graph does not hold this frame's blend. */
	ID3D11ShaderResourceView* MaskSRV() const;

	/** @brief The one decision the views and Terrain Blending's own write follow. A frame whose anchor ran nothing reads the D3D11 textures. */
	bool Holds() const;

	~ORGTerrainBlending();

	struct Impl;

private:
	ORGTerrainBlending() = default;

	/** @brief False, logged, on failure. */
	bool RegisterClient();

	/** @brief For good. Terrain Blending returns to its D3D11 textures. */
	void Fail(const char* a_why);

	/** @brief Set by BeforeDepth, then by Blend. */
	enum class FrameBlend : std::uint8_t
	{
		None,     // the D3D11 textures
		Planned,  // the depth table names the graph's exports, the anchor has not run yet
		Written,
		Missed,   // the anchor submitted nothing, so Terrain Blending blended on D3D11
	};

	std::shared_ptr<Impl> impl;
	std::function<void()> onFailed;
	FrameBlend frame = FrameBlend::None;
	bool failed = false;
	bool programFailed = false;  // a shader reload may fix a failed compile
};

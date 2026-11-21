#pragma once

#include "RenderGraph/Port.h"

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
* @brief Terrain Blending's depth blend on the render graph, in graph mode.
*
* The terrain depth is a persistent, published depth twin exported to D3D11. Near terrain is drawn into it a second time
* without the depth offset, by DCLF or by the engine through the export. Its clear runs at EarlyPrepass.
* One compute pass at DepthDone writes min(main, terrain) as R32 and R16 blended depths, and copies the main depth into the mask.
* All three are exported, because the engine reads the blended depth through the depth table, and published for graph readers.
* They are declared on the display size and their client on Enabled (Frame/Declare.h), so a resize or a toggle switches in
* with a frame revision. The terrain depth and its clear are made once; the clear follows Enabled too.
* In graph mode any failure is fatal. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGTerrainBlending : public RenderGraph::Port<ORGTerrainBlending>
{
public:
	static constexpr std::string_view kName = "Terrain Blending";

	/** @brief The published ids. */
	static constexpr std::string_view kBlendedDepthId = "TerrainBlending::BlendedDepth";
	static constexpr std::string_view kBlendedDepth16Id = "TerrainBlending::BlendedDepth16";
	static constexpr std::string_view kMaskId = "TerrainBlending::Mask";
	static constexpr std::string_view kTerrainDepthId = "TerrainBlending::TerrainDepth";

	/**
	* @brief Call at SetupResources. In graph mode a failure is fatal.
	* @return True when the blend runs on the graph. False outside graph mode, where Terrain Blending uses D3D11.
	*/
	bool Setup();
	/**
	* @brief Call before Setup. Fills a_target with the twin's exports, one reference each as D3D11's Create gives.
	* The twin lives as long as the game. Fatal on failure in graph mode. @return False outside graph mode: make a D3D11 texture.
	*/
	bool MakeTerrainDepth(RE::BSGraphics::DepthStencilData& a_target, const D3D11_TEXTURE2D_DESC& a_desc, DXGI_FORMAT a_srvFormat, DXGI_FORMAT a_dsvFormat);
	/** @brief Null outside graph mode, where Terrain Blending makes a D3D11 one. */
	std::shared_ptr<org::ExternalTextureResource> TerrainDepthGraph() const;
	/** @brief Whether this frame's EarlyPrepass run already cleared it, so the D3D11 clear can be skipped. */
	bool TerrainDepthCleared() const;

	/**
	* @brief At every depth pass, while the depth table holds the engine's own views. Imports the main and the terrain depth;
	* new ones make the next revision register the blend's client for them.
	*/
	void Observe();

	/** @brief At setup's build point (Frame::Revision::OnSetupBuildPoint): Observe, when both depths exist, so the setup build holds the blend. */
	void ObserveAtSetup();

	/**
	* @brief Call before the depth hook points the depth table at the blended depth, in frames that blend.
	* @return False outside graph mode: the D3D11 path blends. In graph mode always true: a frame whose stage lags blends nothing.
	*/
	bool BeforeDepth();

	/** @brief After the z-prepass. @return False outside graph mode: blend on D3D11. A failed run is fatal. */
	bool Blend();

	/** @brief The port's IsActive, for callers that still use this name. */
	bool Active() const { return IsActive(); }

	/** @brief Null when the graph does not hold this frame's blend. */
	ID3D11ShaderResourceView* BlendedDepthSRV(bool a_16bit) const;

	/** @brief t55. Null when the graph does not hold this frame's blend. */
	ID3D11ShaderResourceView* MaskSRV() const;

	/** @brief The one decision the views and Terrain Blending's own write follow. In graph mode every blending frame holds it. */
	bool Holds() const;
	~ORGTerrainBlending();

	struct Impl;

private:
	friend class RenderGraph::Port<ORGTerrainBlending>;
	ORGTerrainBlending();

	/** @brief Set by BeforeDepth, then by Blend. */
	enum class FrameBlend : std::uint8_t
	{
		None,     // the D3D11 textures
		Planned,  // the depth table names the graph's exports, the anchor has not run yet
		Written,
		Missed,   // the stage lagged and the anchor ran nothing: the blended depth keeps the last frame's
	};

	std::shared_ptr<Impl> impl;
	FrameBlend frame = FrameBlend::None;
};

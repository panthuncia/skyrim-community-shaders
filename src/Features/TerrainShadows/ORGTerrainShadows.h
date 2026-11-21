#pragma once

#include "RenderGraph/Port.h"

#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "Features/TerrainShadows.h"

namespace DirectX
{
	struct Image;
}

/**
* @brief Terrain Shadows' shadow update as an EarlyPrepass pass, in graph mode.
*
* The feature decides the stripes and their constants, and this class only runs them. Each heightmap is a new value of the
* HeightMap frame variable (its id and size). The shadow height map is declared on it: a persistent, versioned, published texture
* exported for t60, whose size is a new version made by the frame revision. The client is declared on it too, with the heightmap's
* own texture, upload and gate. Both switch in at the Begin that adopts the revision. Until a full refresh has run in a graph
* holding the new client, NeedsRefresh stays true and the feature keeps terrain shadows off, so nothing reads unwritten contents.
* In graph mode any failure is fatal. Render thread. Free of ORG/Vulkan includes.
*/
class ORGTerrainShadows : public RenderGraph::Port<ORGTerrainShadows>
{
public:
	static constexpr std::string_view kName = "Terrain Shadows";

	/** @brief The shadow height map's published id, which is also its name in both APIs. */
	static constexpr std::string_view kShadowHeightId = "TerrainShadows::ShadowHeight";

	/**
	* @brief At SetupResources with a heightmap installed. Declares the shadow height map and the client. Outside graph mode it
	* does nothing; in graph mode a failure is fatal.
	*/
	void Setup();

	/**
	* @brief Stages a new heightmap and writes the HeightMap variable. It switches in with the frame revision that carries it.
	* @param a_image Mip 0 of the DDS, copied before this returns.
	* @return False when the ORG path is not active. A heightmap it cannot take is fatal.
	*/
	bool SetHeightmap(const DirectX::Image& a_image);

	/**
	* @brief Queues this frame's dispatches, one constants entry each. Entries beyond a full refresh are dropped.
	* @return False when no heightmap is on the graph.
	*/
	bool Submit(std::span<const TerrainShadows::ShadowUpdateCB> a_dispatches, std::uint32_t a_groups, bool a_refresh);

	/** @brief True from SetHeightmap until a full refresh ran in a graph holding that heightmap's client. */
	bool NeedsRefresh() const;

	/** @brief t60. Null while no heightmap is on the graph. */
	ID3D11ShaderResourceView* ShadowHeightSRV() const;
	~ORGTerrainShadows();

	struct Impl;
	struct Heightmap;

private:
	friend class RenderGraph::Port<ORGTerrainShadows>;
	ORGTerrainShadows();

	std::shared_ptr<Impl> impl;
};

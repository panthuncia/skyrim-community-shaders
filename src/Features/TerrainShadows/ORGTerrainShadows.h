#pragma once

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
* @brief Terrain Shadows' shadow update as an EarlyPrepass pass, with CS_ORG_TERRAIN_SHADOWS=1.
*
* The feature decides the stripes and their constants, and this class only runs them. The shadow height map is a persistent,
* versioned, published texture exported for t60. Each heightmap resizes it as a new version, which readers follow with no build.
* Each heightmap registers the client again with its own texture, upload and gate. Until a full refresh has run in a graph holding
* the new registration, NeedsRefresh stays true and the feature keeps terrain shadows off, so nothing reads unwritten contents.
* On any failure the ORG path stops for good and the heightmap loads again on D3D11. Render thread. Free of ORG/Vulkan includes.
*/
class ORGTerrainShadows
{
public:
	/** @brief The shadow height map's published id, which is also its name in both APIs. */
	static constexpr std::string_view kShadowHeightId = "TerrainShadows::ShadowHeight";

	static bool Requested();

	/** @brief Call in PostPostLoad, before the render graph initializes. */
	static void Reserve();

	static ORGTerrainShadows& Get();

	/**
	* @brief At SetupResources with the runtime active and a heightmap installed.
	* @param a_onFailed Called once if a heightmap switch fails later.
	* @return False keeps Terrain Shadows on D3D11.
	*/
	bool Setup(std::function<void()> a_onFailed);

	bool IsActive() const;

	/**
	* @brief Stages a new heightmap. It switches in at the next frame boundary.
	* @param a_image Mip 0 of the DDS, copied before this returns.
	* @return False, logged, on failure. The ORG path is then off for good.
	*/
	bool SetHeightmap(const DirectX::Image& a_image);

	/**
	* @brief Queues this frame's dispatches, one constants entry each. Entries beyond a full refresh are dropped.
	* @return False when no heightmap is on the graph.
	*/
	bool Submit(std::span<const TerrainShadows::ShadowUpdateCB> a_dispatches, std::uint32_t a_groups, bool a_refresh);

	/** @brief True from SetHeightmap until a full refresh ran in a graph holding that heightmap. */
	bool NeedsRefresh() const;

	/** @brief t60. Null while no heightmap is on the graph. */
	ID3D11ShaderResourceView* ShadowHeightSRV() const;

	~ORGTerrainShadows();

	struct Impl;
	struct Heightmap;

private:
	ORGTerrainShadows() = default;

	/** @brief For good. */
	void Stop();

	/** @brief At a frame boundary. Resizes for the staged heightmap and registers it. */
	void Switch();

	std::shared_ptr<Impl> impl;
};

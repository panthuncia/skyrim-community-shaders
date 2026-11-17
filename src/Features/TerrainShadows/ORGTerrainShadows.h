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
 * @brief Terrain Shadows' shadow update as a render-graph pass (CS_ORG_TERRAIN_SHADOWS=1).
 *
 * The update is a client of the EarlyPrepass anchor: ShadowUpdate.cs.hlsl updates one stripe of the shadow height map a frame, or
 * the whole map on a refresh, exactly as the D3D11 path does. The feature decides the stripes and their constants on the render
 * thread (TerrainShadows::UpdateShadow) and hands them over; this class only runs them.
 *
 * Resources:
 * - The shadow height map: a persistent graph texture written by the pass, made at setup and published once (PublishedResources):
 *   Drawcall Limit Fix's colour passes read it through the graph. It is versioned: each heightmap resizes it to its own size as a
 *   new version of the same slot, which the readers follow with no build. It exports a D3D11 SRV, which the feature binds at t60
 *   for the engine's pixel shaders until those draws are ported.
 * - Per heightmap, a registration of the client of its own (Heightmap): the heightmap texture at the heightmap's size, kept in
 *   its DDS format when that is BC4_UNORM or R16_UNORM (any other is converted to R16_UNORM), its staged bytes, a one-shot upload
 *   pass, and a latch and run gate sized for its stripes. A worldspace change registers the client again, so the EarlyPrepass
 *   graphs are built again, staged, and switch in whole. The old registration's gate stays closed until then.
 * - Until a full refresh was committed in a graph that holds the new registration, NeedsRefresh is true: the feature keeps asking
 *   for refreshes and keeps terrain shadows off, so no frame reads the new version before it is written.
 *
 * Setup at SetupResources makes the program and the shadow height map and publishes it. On any failure (a shader that does not
 * compile, a texture that cannot be made, the graph stopping) the ORG path stops for good and the feature loads its heightmap
 * again on D3D11.
 *
 * Everything here runs on the render thread. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGTerrainShadows
{
public:
	/** @brief The shadow height map's published id (PublishedResources), its twin's name, and the D3D11 texture's name. */
	static constexpr std::string_view kShadowHeightId = "TerrainShadows::ShadowHeight";

	/** @brief Whether CS_ORG_TERRAIN_SHADOWS=1 is set. */
	static bool Requested();

	/** @brief Reserves the EarlyPrepass anchor when requested. Call before the render graph initializes (PostPostLoad). */
	static void Reserve();

	static ORGTerrainShadows& Get();

	/**
	 * @brief At SetupResources, with the runtime active and a heightmap installed: makes the program and the shadow height map, and
	 * publishes it.
	 * @param a_onFailed Called once if the graph fails later; the feature loads its heightmap again on D3D11.
	 * @return Whether heightmaps go to the render graph; false keeps Terrain Shadows on D3D11.
	 */
	bool Setup(std::function<void()> a_onFailed);

	/** @brief Whether the ORG path is set up and the runtime is still up: a new heightmap goes to the render graph. */
	bool IsActive() const;

	/**
	 * @brief Replaces the heightmap: makes its texture and stages its bytes. At the next frame boundary the shadow height map is
	 * resized to it (a new version) and the client is registered again with this heightmap's passes.
	 * @param a_image Mip 0 of the DDS. Its bytes are copied before this returns.
	 * @return False, logged, when anything fails; the ORG path is then off for good.
	 */
	bool SetHeightmap(const DirectX::Image& a_image);

	/**
	 * @brief This frame's dispatches, run at the EarlyPrepass anchor: one constants entry a dispatch, in order, each with a_groups
	 * groups. More entries than the heightmap needs for a full refresh are dropped.
	 * @param a_refresh Whether the entries are a full refresh (every stripe, blend weight 1).
	 * @return Whether the update was queued: false when no heightmap is on the graph.
	 */
	bool Submit(std::span<const TerrainShadows::ShadowUpdateCB> a_dispatches, std::uint32_t a_groups, bool a_refresh);

	/**
	 * @brief True from SetHeightmap until a full refresh was committed in a graph that holds that heightmap. The shadow height map's
	 * contents are not the heightmap's until then.
	 */
	bool NeedsRefresh() const;

	/** @brief The shadow height map's exported view (t60); null while no heightmap is on the graph. */
	ID3D11ShaderResourceView* ShadowHeightSRV() const;

	~ORGTerrainShadows();

	/** @brief The program and the shadow height map (Setup), and the current heightmap's registration (ORGTerrainShadows.cpp). */
	struct Impl;
	/** @brief One heightmap's registration of the client (ORGTerrainShadows.cpp). */
	struct Heightmap;

private:
	ORGTerrainShadows() = default;

	/** @brief Stops the ORG path for good. */
	void Stop();

	/** @brief At a frame boundary: the staged heightmap's resize of the shadow height map and its registration. */
	void Switch();

	std::shared_ptr<Impl> impl;
};

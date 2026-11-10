#pragma once

#include <cstdint>
#include <d3d11.h>
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
 * the whole map on a refresh, exactly as the D3D11 path does. The feature still decides the stripes and their constants on the
 * render thread (TerrainShadows::UpdateShadow); this class only runs them.
 *
 * Resources:
 * - The heightmap: a persistent graph texture, owned by the graph. Nothing outside the graph reads it, so it has no D3D11
 *   view. Its DDS bytes go into an upload buffer at once, and a copy pass fills it in the first epoch; the graph is then built
 *   again without that pass. Async epochs record no texture uploads, so the upload service cannot fill it.
 * - The shadow height map: a persistent graph texture written by the pass. It exports a D3D11 SRV, which the feature binds at t60
 *   for the engine's pixel shaders until those draws are ported. It is also published (PublishedResources): Drawcall Limit Fix's
 *   colour passes read it through the graph.
 * - The per-dispatch constants: one LATCH region a dispatch, written by the client's commit at the frame slot.
 *
 * On any failure (no runtime, a shader that does not compile, a texture that cannot be made, the graph stopping) it stops for
 * good and Active() turns false: the feature then loads its heightmap again on its D3D11 path.
 *
 * Everything here runs on the render thread. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGTerrainShadows
{
public:
	/** @brief The shadow height map's published id (PublishedResources), its twin's name. */
	static constexpr std::string_view kShadowHeightId = "TerrainShadows::ShadowHeight";

	/** @brief Whether CS_ORG_TERRAIN_SHADOWS=1 is set. */
	static bool Requested();

	/** @brief Reserves the EarlyPrepass anchor when requested. Call before the render graph initializes (PostPostLoad). */
	static void Reserve();

	static ORGTerrainShadows& Get();

	/** @brief Whether a new heightmap should go to the render graph: requested, not failed, the runtime up and the anchor reserved. */
	bool Available() const;

	/** @brief Whether the current heightmap is on the render graph and its pass is registered. */
	bool Active() const;

	/**
	 * @brief Replaces the heightmap: makes the program (once), the heightmap and shadow height textures and the latch, queues the
	 * heightmap's bytes in an upload buffer and registers the anchor client. The caller unbinds the previous shadow height view first.
	 * @param a_image Mip 0 of the DDS (BC4 or any other typed format). Its bytes are copied before this returns.
	 * @return False, logged, when anything fails; the ORG path is then off for good.
	 */
	bool SetHeightmap(const DirectX::Image& a_image);

	/**
	 * @brief This frame's dispatches, run at the EarlyPrepass anchor: one constants entry a dispatch, in order, each with a_groups
	 * groups. More entries than the heightmap needs for a full refresh are dropped.
	 * @return Whether the update was queued.
	 */
	bool Submit(std::span<const TerrainShadows::ShadowUpdateCB> a_dispatches, std::uint32_t a_groups);

	/** @brief The shadow height map's exported view (t60); null when inactive. */
	ID3D11ShaderResourceView* ShadowHeightSRV() const;

	~ORGTerrainShadows();

	/** @brief The resources and per-frame state the pass and the client share (ORGTerrainShadows.cpp). */
	struct Impl;

private:
	ORGTerrainShadows() = default;

	/** @brief Stops the ORG path for good. */
	void Fail();

	std::shared_ptr<Impl> impl;
	bool failed = false;
};

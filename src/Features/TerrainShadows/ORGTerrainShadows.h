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
 * Resources, all made once at setup so that a worldspace change rebuilds no graph:
 * - Both textures are as large as the largest installed heightmap. A heightmap fills their top-left corner; the readers' UV scale
 *   and the update's PxSize say how much of them it covers.
 * - The heightmap: a persistent R16_UNORM graph texture, owned by the graph. Nothing outside the graph reads it, so it has no D3D11
 *   view. A new heightmap is converted to R16_UNORM, its bytes go into an upload buffer, and the client's copy pass (recorded every
 *   run, copying only on a run whose commit stored the buffer) fills the corner, until a run with it was submitted.
 * - The shadow height map: a persistent graph texture written by the pass. It exports a D3D11 SRV, which the feature binds at t60
 *   for the engine's pixel shaders until those draws are ported. It is published once (PublishedResources): Drawcall Limit Fix's
 *   colour passes read it through the graph.
 * - The per-dispatch constants: one LATCH region a dispatch, and the dispatches' group counts in a run gate, both written by the
 *   client's commit at the frame slot. A run without an update does no work.
 *
 * Setup at SetupResources makes the program, the latch, the gate and the textures, registers the client and publishes the shadow
 * height map. On any failure (a shader that does not compile, a texture that cannot be made, the graph stopping) the ORG path
 * stops for good and the feature loads its heightmap again on D3D11.
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
	 * @brief At SetupResources, with the runtime active: makes the program, the constants' latch, the run gate and both textures at
	 * a_maxWidth x a_maxHeight (the largest installed heightmap), registers the client and publishes the shadow height map.
	 * @param a_onFailed Called once if the graph fails later; the feature loads its heightmap again on D3D11.
	 * @return Whether heightmaps go to the render graph; false keeps Terrain Shadows on D3D11.
	 */
	bool Setup(std::uint32_t a_maxWidth, std::uint32_t a_maxHeight, std::function<void()> a_onFailed);

	/** @brief Whether the ORG path is set up and the runtime is still up: a new heightmap goes to the render graph. */
	bool IsActive() const;

	/**
	 * @brief Replaces the heightmap: converts it to R16_UNORM and queues its bytes for the copy pass, which writes them into the
	 * textures' corner. Registers nothing and publishes nothing.
	 * @param a_image Mip 0 of the DDS (BC4 or any other typed format), at most the size given to Setup. Its bytes are copied before
	 * this returns.
	 * @return False, logged, when anything fails; the ORG path is then off for good.
	 */
	bool SetHeightmap(const DirectX::Image& a_image);

	/**
	 * @brief This frame's dispatches, run at the EarlyPrepass anchor: one constants entry a dispatch, in order, each with a_groups
	 * groups. More entries than the heightmap needs for a full refresh are dropped.
	 * @return Whether the update was queued: false when the current heightmap is not the graph's.
	 */
	bool Submit(std::span<const TerrainShadows::ShadowUpdateCB> a_dispatches, std::uint32_t a_groups);

	/** @brief The shadow height map's exported view (t60); null while no heightmap is on the graph. */
	ID3D11ShaderResourceView* ShadowHeightSRV() const;

	/**
	 * @brief The shadow height map's full size while a heightmap is on the graph: the heightmap covers only its corner, so readers
	 * scale their UVs by the heightmap's size over this.
	 * @return False while no heightmap is on the graph.
	 */
	bool TextureSize(std::uint32_t& a_width, std::uint32_t& a_height) const;

	~ORGTerrainShadows();

	/** @brief The program, latch, gate and textures (Setup) and the heightmap waiting to be copied (ORGTerrainShadows.cpp). */
	struct Impl;

private:
	ORGTerrainShadows() = default;

	/** @brief Stops the ORG path for good. */
	void Stop();

	std::shared_ptr<Impl> impl;
};

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
 * - The heightmap: a persistent graph texture, owned by the graph. Nothing outside the graph reads it, so it has no D3D11
 *   view. Its DDS bytes go into an upload buffer at once, and a one-shot upload pass fills it at the anchor's next run.
 * - The shadow height map: a persistent graph texture written by the pass. It exports a D3D11 SRV, which the feature binds at t60
 *   for the engine's pixel shaders until those draws are ported. It is also published (PublishedResources): Drawcall Limit Fix's
 *   colour passes read it through the graph.
 * - The per-dispatch constants: one LATCH region a dispatch, and the dispatches' group counts in a run gate, both written by the
 *   client's commit at the frame slot. A run without an update does no work.
 *
 * Setup at SetupResources makes the program, the latch and the gate. Each heightmap then makes its textures and registers the
 * client again. On any failure (a shader that does not compile, a texture that cannot be made, the graph stopping) the ORG path
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
	 * @brief At SetupResources, with the runtime active: makes the program, the constants' latch and the run gate.
	 * @param a_onFailed Called once if the graph fails later; the feature loads its heightmap again on D3D11.
	 * @return Whether heightmaps go to the render graph; false keeps Terrain Shadows on D3D11.
	 */
	bool Setup(std::function<void()> a_onFailed);

	/** @brief Whether the ORG path is set up and the runtime is still up: a new heightmap goes to the render graph. */
	bool IsActive() const;

	/**
	 * @brief Replaces the heightmap: makes the heightmap and shadow height textures, queues the heightmap's bytes in an upload buffer
	 * and registers the anchor client again. The caller unbinds the previous shadow height view first.
	 * @param a_image Mip 0 of the DDS (BC4 or any other typed format). Its bytes are copied before this returns.
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

	~ORGTerrainShadows();

	/** @brief The program, latch and gate (Setup) and the current heightmap's textures (ORGTerrainShadows.cpp). */
	struct Impl;

private:
	ORGTerrainShadows() = default;

	/** @brief Stops the ORG path for good. */
	void Stop();

	std::shared_ptr<Impl> impl;
};

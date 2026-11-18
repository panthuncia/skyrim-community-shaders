#pragma once

#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

struct Skylighting;
class Texture2D;

namespace org
{
	class ExternalTextureResource;
}

/**
 * @brief Skylighting's per-frame probe update (UpdateProbesCS) as a render-graph compute pass at the Prepass anchor, behind
 * CS_ORG_SKYLIGHTING=1.
 *
 * The four probe volumes are graph textures with History lifetime. They are exported to D3D11 (SRV): the engine's draws bind the
 * probe array and the shadow visibility at t50 and t53, and the deferred composite reads them through Skylighting. ResetSkylighting
 * clears them with one-shot graph passes (QueueReset), ordered with the graph's reads. The probe array and the shadow visibility are
 * published (PublishedResources): Exponential Height Fog's passes and Drawcall Limit Fix's colour passes read them by id.
 *
 * The sky occlusion map is a graph-owned depth texture (MakeOcclusionMap, "Skylighting::Occlusion", persistent, published) exported
 * to D3D11 (SRV and DSV). Its writers: the engine's RenderMask draws into the DSV (Skylighting swaps it in for the precipitation
 * occlusion target), or Drawcall Limit Fix's shadow epoch through the same graph resource (OcclusionMap), after a D3D11 clear of the
 * DSV (Skylighting::RenderSkyOcclusionMap). The pass reads it by that resource, and the directional shadow maps as imported DXVK
 * images, CS's shared constants as the D3D11 dispatch does, and the directional shadow data from a latch. A run gate holds its group counts: a frame without an update (an interior, the map menu)
 * dispatches nothing.
 *
 * Everything here runs on the render thread.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGSkylighting
{
public:
	/** @brief The published ids (PublishedResources), the volumes' names: the probe array and the shadow visibility. */
	static constexpr std::string_view kProbeArrayId = "Skylighting::ProbeArray";
	static constexpr std::string_view kShadowVisibilityId = "Skylighting::ShadowVisibility";
	/** @brief The other two volumes' names, which are not published. */
	static constexpr std::string_view kAccumFramesId = "Skylighting::AccumFramesArray";
	static constexpr std::string_view kShadowBitmaskId = "Skylighting::ShadowBitmask";
	/** @brief The sky occlusion map's published id and name. */
	static constexpr std::string_view kOcclusionId = "Skylighting::Occlusion";

	/** @brief The probe volumes, in UpdateProbesCS's UAV order. */
	enum class Volume : std::uint32_t
	{
		ProbeArray,
		AccumFrames,
		ShadowBitmask,
		ShadowVisibility,
		Count
	};

	/** @brief Whether CS_ORG_SKYLIGHTING=1 asks for the render-graph path. */
	static bool Requested();

	/** @brief Before the render graph initializes (Skylighting's PostPostLoad), when requested: reserves the Prepass anchor. */
	static void Reserve();

	static ORGSkylighting& Get();

	/**
	 * @brief At SetupResources, with the runtime active: makes the program, sampler, latch, gate and volumes (a_feature's probe grid)
	 * and registers the anchor client, whose one-shot passes reset the volumes at its first run.
	 * @return Whether the probe update runs on the render graph; false keeps Skylighting on D3D11.
	 */
	bool Setup(const Skylighting& a_feature);

	/**
	 * @brief At SetupResources, before Setup, when requested and the runtime is active: makes the sky occlusion map as a graph-owned
	 * depth twin of a_desc (the precipitation occlusion target's description, typeless), exported (SRV, DSV) and published as
	 * kOcclusionId. a_srvFormat and a_dsvFormat are the target's views' formats, which the twin's must match. It lives as long as the game, whatever happens to the probe update, since D3D11 keeps drawing into its export.
	 * @return A Buffer.h wrapper of the export (its texture, SRV and DSV) for Skylighting's texOcclusion; null: make a D3D11 one.
	 */
	Texture2D* MakeOcclusionMap(const D3D11_TEXTURE2D_DESC& a_desc, DXGI_FORMAT a_srvFormat, DXGI_FORMAT a_dsvFormat);

	/** @brief The sky occlusion map's graph resource (MakeOcclusionMap); null when Skylighting made a D3D11 one. */
	std::shared_ptr<org::ExternalTextureResource> OcclusionMap() const;

	/**
	 * @brief Render thread, at Skylighting's Prepass, on a frame that updates the probes: imports this frame's inputs and opens the
	 * update for the Prepass anchor's run, with the frame's directional shadow data. Without an input the volumes hold this frame.
	 * When Drawcall Limit Fix drew the occlusion map (NoteOcclusionDrawn), the same run then clears the map (a twin's only) after the
	 * update read it, for DCLF's next draw.
	 * @return Whether the ORG path is active; false: the D3D11 dispatch runs.
	 */
	bool Update(const Skylighting& a_feature);

	/** @brief Render thread: Drawcall Limit Fix's shadow epoch draws the occlusion map this frame (RenderSkyOcclusionMap). */
	void NoteOcclusionDrawn();

	/**
	 * @brief Render thread, before the map is drawn: whether the graph cleared it since its last draw (Update's clear), so no D3D11
	 * clear is needed. Taken: false until the next clear. Call it for every draw of the map, so an engine draw also uses it up.
	 */
	bool TakeOcclusionCleared();

	/** @brief Whether the probe update runs on the render graph: set up, and the runtime still up. */
	bool IsActive() const;

	/** @brief A volume's exported D3D11 view; null when inactive. */
	ID3D11ShaderResourceView* SRV(Volume a_volume) const;

	/**
	 * @brief Render thread: the volumes reset to ResetSkylighting's starting values by one-shot clear passes before the probe update at
	 * the Prepass anchor's next run (FrameAnchors::Rearm).
	 * @return False when the ORG path is not active: the D3D11 volumes are Skylighting's to clear.
	 */
	bool QueueReset();

	~ORGSkylighting();

private:
	ORGSkylighting() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};

#pragma once

#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

struct Skylighting;

/**
 * @brief Skylighting's per-frame probe update (UpdateProbesCS) as a render-graph compute pass at the Prepass anchor, behind
 * CS_ORG_SKYLIGHTING=1.
 *
 * The four probe volumes are graph textures with History lifetime. They are exported to D3D11 (SRV): the engine's draws bind the
 * probe array and the shadow visibility at t50 and t53, and the deferred composite reads them through Skylighting. ResetSkylighting
 * clears them with one-shot graph passes (QueueReset), ordered with the graph's reads. The probe array and the shadow visibility are
 * published (PublishedResources): Exponential Height Fog's passes and Drawcall Limit Fix's colour passes read them by id.
 *
 * The pass reads the sky occlusion map (Skylighting's own depth texture, which the engine or Drawcall Limit Fix renders before the
 * Prepass) and the directional shadow maps as imported DXVK images, CS's shared constants as the D3D11 dispatch does, and the
 * directional shadow data from a latch. A run gate holds its group counts: a frame without an update (an interior, the map menu)
 * dispatches nothing.
 *
 * Everything here runs on the render thread. When the graph fails, the client is dropped and Skylighting returns to its D3D11 dispatch.
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
	 * @param a_onFailed Called once if the graph fails later; Skylighting returns to D3D11.
	 * @return Whether the probe update runs on the render graph; false keeps Skylighting on D3D11.
	 */
	bool Setup(const Skylighting& a_feature, std::function<void()> a_onFailed);

	/**
	 * @brief Render thread, at Skylighting's Prepass, on a frame that updates the probes: imports this frame's inputs and opens the
	 * update for the Prepass anchor's run, with the frame's directional shadow data. Without an input the volumes hold this frame.
	 * @return Whether the ORG path is active; false: the D3D11 dispatch runs.
	 */
	bool Update(const Skylighting& a_feature);

	/** @brief Whether the probe update runs on the render graph: set up, not failed, and the runtime still up. */
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

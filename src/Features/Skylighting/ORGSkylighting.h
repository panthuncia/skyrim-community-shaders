#pragma once

#include <cstdint>
#include <d3d11.h>
#include <memory>

struct Skylighting;

/**
 * @brief Skylighting's per-frame probe update (UpdateProbesCS) as a render-graph compute pass at the Prepass anchor, behind
 * CS_ORG_SKYLIGHTING=1.
 *
 * The four probe volumes are graph textures with History lifetime. They are exported to D3D11 (SRV): the engine's draws bind the
 * probe array and the shadow visibility at t50 and t53, and the deferred composite reads them through Skylighting. ResetSkylighting
 * clears them with a graph pass (QueueReset), ordered with the graph's reads. Exponential Height Fog's passes read the probe array by
 * name; the probe array and the shadow
 * visibility are also published (PublishedResources), and Drawcall Limit Fix's colour passes read them so. The pass reads the sky occlusion map (Skylighting's own depth
 * texture, which the engine or Drawcall Limit Fix renders before the Prepass) and the directional shadow maps as imported DXVK images, and
 * the frame's values from a latch.
 *
 * Everything here runs on the render thread. When the graph fails, the client is dropped and Skylighting returns to its D3D11 dispatch.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGSkylighting
{
public:
	/** @brief The probe volumes, in UpdateProbesCS's UAV order. */
	enum class Volume : std::uint32_t
	{
		ProbeArray,
		AccumFrames,
		ShadowBitmask,
		ShadowVisibility,
		Count
	};

	/** @brief Whether CS_ORG_SKYLIGHTING=1 is set. */
	static bool Requested();

	/** @brief Before the render graph initializes (Skylighting's PostPostLoad), when requested: reserves the Prepass anchor. */
	static void Reserve();

	static ORGSkylighting& Get();

	/**
	 * @brief Render thread, at Skylighting's Prepass, on a frame that updates the probes: sets the ORG path up on its first call (once
	 * the shadow maps are known) and queues this frame's values (a_feature's grid, the frame's camera and shadow cascades) for the
	 * Prepass anchor's epoch.
	 * @return Whether the ORG path does this frame's update; false: the D3D11 dispatch does.
	 */
	bool Update(const Skylighting& a_feature);

	/** @brief Whether the ORG path is set up and the graph runs: the volumes are the graph's. */
	bool IsActive() const;

	/** @brief A volume's exported D3D11 view; null when inactive. */
	ID3D11ShaderResourceView* SRV(Volume a_volume) const;

	/**
	 * @brief Render thread: the volumes reset to ResetSkylighting's starting values by a clear pass before the probe update at the Prepass
	 * anchor. The pass is in the anchor's builds from the next one until it has been prepared once.
	 * @return False when the ORG path is not active: the D3D11 volumes are Skylighting's to clear.
	 */
	bool QueueReset();

	~ORGSkylighting();

private:
	ORGSkylighting() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
	// Set up once: a failure (or the graph's) keeps the D3D11 path for the session.
	bool attempted = false;
};

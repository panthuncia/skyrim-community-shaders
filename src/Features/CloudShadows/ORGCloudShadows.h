#pragma once

#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

namespace GraphResources
{
	class Texture2D;
}

namespace org
{
	class ExternalTextureResource;
}

/**
 * @brief Cloud Shadows' occlusion copy as a render-graph pass (CS_ORG_CLOUD_SHADOWS=1).
 *
 * What Cloud Shadows draws and copies:
 * - Each cloud layer drawn into the reflections cubemap also writes its occlusion into one of Cloud Shadows' 32 layer cubemaps (a 4th
 *   render target the feature adds). Layer L of a face holds the occlusion of the face's layers up to L: the main pass's clouds read
 *   their own layer for self shadowing (t26). With Drawcall Limit Fix's sky family drawing the faces' clouds (CS_ORG_SKY), the
 *   graph writes the same layers through their imports (ImportLayer).
 * - Between those draws, Cloud Shadows clears and copies layer faces (CheckResourcesSide, SkyShaderHacks, and PropagateToCompletion
 *   when the next face starts). These run inside the engine's draw stream, where there is no anchor, so they stay on D3D11.
 * - At EarlyPrepass, PropagateToCompletion fills the faces the frame did not draw. Its copies change every frame, chain into each
 *   other, and read and write the same layer textures. They stay on D3D11 with the layers (see the API notes).
 * - At the reflections prepass, the last layer is copied into the occlusion cubemap that every reader samples. This copy is the
 *   graph pass here.
 *
 * The ReflectionsPrepass anchor runs at every cubemap render, and the copy happens only on the frame's first, as on D3D11. A copy
 * has no indirect form, so the pass is a compute copy (OcclusionCopyCS, one dispatch a face and mip) through a run gate: the commit of
 * the run that copies opens it, and every other run dispatches no groups. The recording stays the same.
 *
 * Resources:
 * - The occlusion cubemap: a graph texture named "CloudShadows::Occlusion", kept across frames. It exports a D3D11 SRV, which the
 *   feature binds at t25 (engine pixel shaders, D3D11 compute) and t26 (a face's cloud self shadowing), and which Drawcall Limit Fix
 *   reads as its frame texture, until those readers are ported. Graph readers (Exponential Height Fog, the sky family) read it by name.
 * - The layers: Cloud Shadows' D3D11 textures, imported with their SRV and the RTV of their first face (ImportLayer), so the copy and
 *   the sky family share one graph resource per layer.
 *
 * On any failure it stops for good and IsActive() turns false: the feature then copies on D3D11 again.
 *
 * Everything here runs on the render thread. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGCloudShadows
{
public:
	/** @brief The occlusion cubemap's published id (PublishedResources), its twin's name. */
	static constexpr std::string_view kOcclusionId = "CloudShadows::Occlusion";

	/** @brief Whether CS_ORG_CLOUD_SHADOWS=1 is set. */
	static bool Requested();

	/** @brief Reserves the ReflectionsPrepass anchor when requested. Call before the render graph initializes (PostPostLoad). */
	static void Reserve();

	static ORGCloudShadows& Get();

	/**
	 * @brief Cloud Shadows' layer a_layer (0 to 31) as a graph resource ("cs.d3d11.CloudShadows::Layer[<layer>]"): its SRV and its faces'
	 * RTVs. Every graph user imports a layer through this, so one import serves them all. Null when the feature has no layers or the
	 * runtime is not active.
	 */
	static std::shared_ptr<org::ExternalTextureResource> ImportLayer(int a_layer);

	/**
	 * @brief At SetupResources, after Cloud Shadows made its layers, with the runtime active: makes the program and the occlusion
	 * cubemap, imports the last layer, registers the anchor client and publishes the cubemap.
	 * @param a_onFailed Called once if the graph fails later; the feature copies and binds its D3D11 occlusion again.
	 * @return Whether the copy runs on the render graph; false keeps it on D3D11.
	 */
	bool Setup(std::function<void()> a_onFailed);

	/** @brief Whether the occlusion cubemap is the graph's and its pass is registered, with the runtime still up. */
	bool IsActive() const;

	/**
	 * @brief Queues this frame's copy of the last layer into the occlusion cubemap, which the ReflectionsPrepass anchor runs right
	 * after the features' reflection prepasses.
	 * @return False when the copy stays on D3D11 (not set up, failed, or the runtime is not up).
	 */
	bool QueueOcclusionCopy();

	/** @brief The occlusion cubemap's exported view (t25, t26); null when inactive. */
	ID3D11ShaderResourceView* OcclusionSRV() const;

	~ORGCloudShadows();

	/** @brief The resources and per-frame state the pass and the client share (ORGCloudShadows.cpp). */
	struct Impl;

private:
	ORGCloudShadows() = default;

	/** @brief Stops the ORG path for good. */
	void Stop();

	std::shared_ptr<Impl> impl;
};

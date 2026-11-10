#pragma once

#include <d3d11.h>
#include <memory>
#include <string_view>

/**
 * @brief Cloud Shadows' occlusion copy as a render-graph pass (CS_ORG_CLOUD_SHADOWS=1).
 *
 * What Cloud Shadows draws and copies:
 * - The engine draws each cloud layer into the reflections cubemap. Cloud Shadows adds a 4th render target to that draw (one of its
 *   32 layer cubemaps). This draw stays on D3D11 until the sky's draws move to the graph (Phase 5).
 * - Between those draws, Cloud Shadows clears and copies layer faces (CheckResourcesSide, SkyShaderHacks, and PropagateToCompletion
 *   when the next face starts). These run inside the engine's draw stream, where there is no anchor, so they stay on D3D11.
 * - At EarlyPrepass, PropagateToCompletion fills the faces the frame did not draw. Its copies change every frame, chain into each
 *   other, and read and write the same layer textures. They stay on D3D11 with the layers (see the API notes).
 * - At the reflections prepass, the last layer is copied into the occlusion cubemap that every reader samples. This copy is the
 *   graph pass here.
 *
 * Resources:
 * - The occlusion cubemap: a graph texture named "CloudShadows::Occlusion", kept across frames. It exports a D3D11 SRV, which the
 *   feature binds at t25 (engine pixel shaders, D3D11 compute) and t26 (cloud self shadowing), and which Drawcall Limit Fix reads as
 *   its frame texture, until those readers are ported. Graph readers (Exponential Height Fog) read it by name.
 * - The last layer: Cloud Shadows' D3D11 texture, imported (FrameAnchors::D3D11Texture).
 *
 * On any failure it stops for good and Active() turns false: the feature then copies on D3D11 again.
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
	 * @brief Queues this frame's copy of the last layer into the occlusion cubemap, which the ReflectionsPrepass anchor runs right
	 * after the features' reflection prepasses. Makes the texture and registers the client the first time.
	 * @return False when the copy stays on D3D11 (not requested, failed, or the runtime is not up).
	 */
	bool QueueOcclusionCopy();

	/** @brief Whether the occlusion cubemap is the graph's and its pass is registered. */
	bool Active() const;

	/** @brief The occlusion cubemap's exported view (t25, t26); null when inactive. */
	ID3D11ShaderResourceView* OcclusionSRV() const;

	~ORGCloudShadows();

	/** @brief The resources and per-frame state the pass and the client share (ORGCloudShadows.cpp). */
	struct Impl;

private:
	ORGCloudShadows() = default;

	/** @brief Makes the texture, imports the last layer and registers the client. False, logged, on failure. */
	bool Setup();

	/** @brief Stops the ORG path for good. */
	void Fail();

	std::shared_ptr<Impl> impl;
	bool failed = false;
};

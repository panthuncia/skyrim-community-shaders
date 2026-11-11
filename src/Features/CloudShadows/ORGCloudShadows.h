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

/**
 * @brief Cloud Shadows' occlusion copy as a render-graph pass (CS_ORG_CLOUD_SHADOWS=1).
 *
 * What Cloud Shadows draws and copies:
 * - The engine draws each cloud layer into the reflections cubemap. Cloud Shadows adds a 4th render target to that draw (one of its
 *   32 layer cubemaps). With Drawcall Limit Fix's sky family on the faces (CS_ORG_SKY=1), the graph draws the faces' clouds instead,
 *   and their occlusion goes into a graph texture of this class's, "CloudShadows::Faces" (one slice a face, the last layer's
 *   contents), which the sky's cloud draws write as their second target.
 * - Between those draws, Cloud Shadows clears and copies layer faces (CheckResourcesSide, SkyShaderHacks, and PropagateToCompletion
 *   when the next face starts). These run inside the engine's draw stream, where there is no anchor, so they stay on D3D11.
 * - At EarlyPrepass, PropagateToCompletion fills the faces the frame did not draw. Its copies change every frame, chain into each
 *   other, and read and write the same layer textures. They stay on D3D11 with the layers (see the API notes).
 * - At the reflections prepass, the last layer is copied into the occlusion cubemap that every reader samples. This copy is the
 *   graph pass here.
 *
 * The ReflectionsPrepass anchor runs at every cubemap render, and the copy happens only on the frame's first, as on D3D11. A copy
 * has no indirect form, so the pass is a compute copy (OcclusionCopyCS, one dispatch a face, mip and source) through a run gate: the
 * commit of the run that copies opens it, and every other run dispatches no groups. The recording stays the same.
 *
 * Each face is copied from the source that drew its clouds last (NoteFace): the D3D11 layer when the engine drew them (Cloud
 * Shadows' SkyShaderHacks), the graph faces when the sky family did. Both sources are declared from the first build, and the choice
 * is the gate's data, so a source changing never rebuilds the graph. (A copy that switched its declared source per batch rebuilt
 * the graph a few hundred times a minute and ran the device out of memory.)
 *
 * Resources:
 * - The occlusion cubemap: a graph texture named "CloudShadows::Occlusion", kept across frames. It exports a D3D11 SRV, which the
 *   feature binds at t25 (engine pixel shaders, D3D11 compute) and t26 (cloud self shadowing), and which Drawcall Limit Fix reads as
 *   its frame texture, until those readers are ported. Graph readers (Exponential Height Fog) read it by name.
 * - The last layer: Cloud Shadows' D3D11 texture, imported (FrameAnchors::D3D11Texture).
 * - The graph faces (only when the sky family asked for them, RequestGraphFaces): a 6-slice array of the layers' shape, kept across
 *   frames and published ("CloudShadows::Faces"). The sky family registers it with its faces' graph and draws into it.
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

	/** @brief The graph faces' published id (RequestGraphFaces), its twin's name. */
	static constexpr std::string_view kFacesId = "CloudShadows::Faces";

	/** @brief Which texture holds a reflection face's latest cloud occlusion. */
	enum class FaceSource : std::uint8_t
	{
		Layer,  // Cloud Shadows' last D3D11 layer: the engine drew the face's clouds
		Graph,  // the graph faces (kFacesId): the sky family drew them
	};

	/** @brief Whether CS_ORG_CLOUD_SHADOWS=1 is set. */
	static bool Requested();

	/** @brief Reserves the ReflectionsPrepass anchor when requested. Call before the render graph initializes (PostPostLoad). */
	static void Reserve();

	/**
	 * @brief Drawcall Limit Fix's sky family draws the reflection faces' clouds: Setup also makes the graph faces and the copy reads
	 * them. Call before Setup (the family's install, at PostPostLoad). Fixed for the session.
	 */
	static void RequestGraphFaces();

	static ORGCloudShadows& Get();

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

	/** @brief The graph faces (kFacesId) the sky family draws the faces' occlusion into; null when not requested or inactive. */
	GraphResources::Texture2D* GraphFaces() const;

	/**
	 * @brief Notes which texture now holds a_face's cloud occlusion; the next copy reads that face from it. Render thread. A graph
	 * source is kept only while the graph faces exist. Does not rebuild anything.
	 */
	void NoteFace(std::uint32_t a_face, FaceSource a_source);

	/** @brief NoteFace for every face (a weather without cloud layers, whose D3D11 last layer was cleared). */
	void NoteAllFaces(FaceSource a_source);

	~ORGCloudShadows();

	/** @brief The resources and per-frame state the pass and the client share (ORGCloudShadows.cpp). */
	struct Impl;

private:
	ORGCloudShadows() = default;

	/** @brief Stops the ORG path for good. */
	void Stop();

	std::shared_ptr<Impl> impl;
};

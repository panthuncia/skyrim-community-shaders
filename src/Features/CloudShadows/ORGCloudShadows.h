#pragma once

#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Texture2D;

namespace GraphResources
{
	class Texture2D;
}

namespace org
{
	class ExternalTextureResource;
}

namespace FrameAnchors
{
	class Texture;
}

/**
 * @brief Cloud Shadows' occlusion copy as a render-graph pass (CS_ORG_CLOUD_SHADOWS=1).
 *
 * What Cloud Shadows draws and copies:
 * - Each cloud layer drawn into the reflections cubemap also writes its occlusion into one of Cloud Shadows' 32 layer cubemaps (a 4th
 *   render target the feature adds). Layer L of a face holds the occlusion of the face's layers up to L: the main pass's clouds read
 *   their own layer for self shadowing (t26). With Drawcall Limit Fix's sky family drawing the faces' clouds (CS_ORG_SKY), the
 *   graph writes the same layers through their imports (ImportLayer).
 * - Between those draws, Cloud Shadows clears and copies layer faces (CheckResourcesSide, NoteGraphCloudLayer, and
 *   PropagateToCompletion when the next face starts). For the faces the sky family draws on the graph, these are layer operations
 *   (below) at the SkyReflections anchor, before the face's draws. For the faces the engine draws (SkyShaderHacks), they stay on
 *   D3D11, inside the engine's draw stream, where there is no anchor.
 * - At EarlyPrepass, PropagateToCompletion fills the faces the frame did not draw: layer operations at the EarlyPrepass anchor.
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
 * - The layers: graph-owned cube twins ("CloudShadows::Layer[<layer>]", MakeLayer), persistent and published, exported to D3D11:
 *   Cloud Shadows makes its SRV and per-face RTVs on the export for the engine's cloud draws and its own clears and copies, and the
 *   copy and the sky family declare the twin itself (ImportLayer). When a twin cannot be made, the layer is a D3D11 texture,
 *   imported with its SRV and the RTV of its first face.
 *
 * Layer operations (client "cs.cloud-shadows.layers", when every layer is a twin):
 * - Cloud Shadows queues each clear and copy of a layer face (QueueLayerOp) for the next run of the anchor that follows the call:
 *   ReflectionsPrepass (the last layer cleared when there are no clouds), SkyReflections (a graph-drawn face's clears and the
 *   previous face's propagation) or EarlyPrepass (the frame's last propagation).
 * - The queue resolves chains as it goes. Each layer face ends up untouched, zero, or a copy of a face as it was before the run.
 * - The run does two gated passes, each one dispatch per layer (LayerOpsCS). The gather copies the faces that are read into a
 *   scratch array. The scatter writes every face that changes, from its scratch slot or as zero. All reads come before all writes,
 *   so the D3D11 calls' order is kept, and each pass needs only one declaration.
 * - Ops stay queued only until that run. Before any D3D11 use of the layers (an engine draw, a D3D11 copy), the feature takes
 *   what is queued and does it on D3D11 first.
 *
 * When setup fails IsActive() stays false and the feature copies on D3D11.
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
	 * @brief Cloud Shadows' layer a_layer (0 to 31) as a graph resource: its twin (MakeLayer), else its D3D11 texture imported
	 * ("cs.d3d11.CloudShadows::Layer[<layer>]") with its SRV and its faces' RTVs. Every graph user takes a layer through this, so one
	 * resource serves them all. Null when the feature has no layers or the runtime is not active.
	 */
	static FrameAnchors::Texture ImportLayer(int a_layer);

	/**
	 * @brief Layer a_layer's published id (its twin's name, "CloudShadows::Layer[<layer>]"), for a reader that declares the twin by id
	 * and reads it by that id's index. Empty when the layer has no twin (a D3D11 layer) or the runtime is not active.
	 */
	static std::string PublishedLayerId(int a_layer);

	/**
	 * @brief At SetupResources, when requested and the runtime is active: makes layer a_layer as a graph-owned cube twin of a_desc
	 * (the layers' description: the reflections cubemap's shape in R8), persistent, exported and published under a_name. It lives as
	 * long as the game, whatever happens to the copy, since D3D11 keeps drawing into its export.
	 * @return A Buffer.h wrapper of the export with an SRV made from a_srvDesc, for Cloud Shadows' texCloudShadowLayers (which makes
	 * the faces' RTVs on it); null: make a D3D11 texture.
	 */
	static Texture2D* MakeLayer(int a_layer, const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_SHADER_RESOURCE_VIEW_DESC& a_srvDesc, const char* a_name);

	/**
	 * @brief At SetupResources, after Cloud Shadows made its layers, with the runtime active: makes the program and the occlusion
	 * cubemap, imports the last layer, registers the anchor client and publishes the cubemap.
	 * @return Whether the copy runs on the render graph; false keeps it on D3D11.
	 */
	bool Setup();

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

	/** @brief Where a layer operation is called from: the anchor whose next run does it, or D3D11 (an engine-drawn face). */
	enum class LayerOpPoint : std::uint8_t
	{
		D3D11,
		ReflectionsPrepass,
		SkyReflections,
		EarlyPrepass,
	};

	/** @brief One layer operation: face `face` of layer `dst` cleared (src == kClearOp), or copied from the same face of layer `src`. */
	struct LayerOp
	{
		std::uint8_t dst = 0;
		std::uint8_t src = 0;
		std::uint8_t face = 0;
	};
	static constexpr std::uint8_t kClearOp = 0xFF;

	/**
	 * @brief Queues a layer operation for the next run of a_point's anchor. Render thread.
	 * @return False: the caller does it on D3D11, after taking and doing what is queued (TakeLayerOps). That happens when the layer
	 * operations are not on the graph, a_point is D3D11, its anchor cannot run the client now, operations for another point are still
	 * queued (their run did not happen), or the scratch array is full.
	 */
	bool QueueLayerOp(LayerOpPoint a_point, LayerOp a_op);

	/** @brief The queued layer operations in call order, taken off the queue, for the caller to do on D3D11. Render thread. */
	std::vector<LayerOp> TakeLayerOps();

	~ORGCloudShadows();

	/** @brief The resources and per-frame state the pass and the client share (ORGCloudShadows.cpp). */
	struct Impl;

	/** @brief The layer operations' resources and queue (ORGCloudShadows.cpp). */
	struct LayerOps;

private:
	ORGCloudShadows() = default;

	/** @brief Stops the ORG path for good. */
	void Stop();

	/** @brief At Setup, with every layer a twin: the layer operations' programs, scratch array and client. False keeps them on D3D11. */
	bool SetupLayerOps();

	std::shared_ptr<Impl> impl;
	std::shared_ptr<LayerOps> layerOps;
};

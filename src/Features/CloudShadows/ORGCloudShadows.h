#pragma once

#include "RenderGraph/Port.h"

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
* @brief Cloud Shadows' occlusion copy and layer operations on the render graph, in graph mode.
*
* Each cloud layer drawn into a reflection face also writes its occlusion into one of 32 layer cubemaps. At the first
* reflections prepass of a frame the last layer is copied into the occlusion cubemap every reader samples. A copy has no
* indirect form, so this is a gated compute copy and the recording stays the same.
* The clears and copies of layer faces between draws are queued as layer operations and run at the next anchor as a gather
* pass then a scatter pass. Before any D3D11 use of the layers, whatever is queued is done on D3D11 first.
* Graph mode never draws a face on D3D11. NoteD3D11Work raises a tap error when one comes.
* In graph mode a setup failure, and any other refusal of the occlusion copy or a layer operation, is fatal. Render thread
* throughout. Free of ORG/Vulkan includes.
*/
class ORGCloudShadows : public RenderGraph::Port<ORGCloudShadows>
{
public:
	static constexpr std::string_view kName = "Cloud Shadows";

	/** @brief The occlusion cubemap's published id. */
	static constexpr std::string_view kOcclusionId = "CloudShadows::Occlusion";

	/** @brief Every graph user takes a layer through this: its twin, so one resource serves them all. Null before the layer is made. */
	static FrameAnchors::Texture ImportLayer(int a_layer);

	/** @brief Empty before the layer is made. */
	static std::string PublishedLayerId(int a_layer);

	/**
	* @brief Makes a persistent, exported, published cube twin for a layer. It lives as long as the game, since D3D11 keeps drawing into its export.
	* @return A wrapper with an SRV for Cloud Shadows, the same one when made again. Null outside graph mode: make a D3D11 texture.
	* In graph mode a failure is fatal.
	*/
	static Texture2D* MakeLayer(int a_layer, const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_SHADER_RESOURCE_VIEW_DESC& a_srvDesc, const char* a_name);

	/** @brief Call at SetupResources after the layers exist. @return Whether the graph path runs: false outside graph mode. In graph mode a failure is fatal. */
	bool Setup();

	/**
	* @brief The ReflectionsPrepass anchor runs it right after the features' reflection prepasses.
	* @return False outside graph mode, where the copy is on D3D11. In graph mode true, or fatal before setup.
	*/
	bool QueueOcclusionCopy();

	/** @brief t25 and t26. Null when inactive. */
	ID3D11ShaderResourceView* OcclusionSRV() const;

	/** @brief The anchor whose next run does the operation, or D3D11 for an engine-drawn face. */
	enum class LayerOpPoint : std::uint8_t
	{
		D3D11,
		ReflectionsPrepass,
		SkyReflections,
		EarlyPrepass,
	};

	/** @brief Clears face of dst when src == kClearOp, else copies the same face from src. */
	struct LayerOp
	{
		std::uint8_t dst = 0;
		std::uint8_t src = 0;
		std::uint8_t face = 0;
	};
	static constexpr std::uint8_t kClearOp = 0xFF;

	/**
	* @brief Render thread. False means the caller does it on D3D11 after doing what TakeLayerOps returns: outside graph mode,
	* or for an engine-drawn face (point D3D11). In graph mode every other refusal is fatal: no setup, an op out of range,
	* another anchor's ops still queued, no scheduled run of the anchor, or a full scratch array.
	*/
	bool QueueLayerOp(LayerOpPoint a_point, LayerOp a_op);

	/** @brief In call order, for the caller to do on D3D11. Render thread. */
	std::vector<LayerOp> TakeLayerOps();

	/** @brief In graph mode, counts this frame as one where Cloud Shadows' work ran on D3D11 (GraphFrames). Render thread. */
	static void NoteD3D11Work();
	~ORGCloudShadows();

	struct Impl;

	struct LayerOps;

private:
	friend class RenderGraph::Port<ORGCloudShadows>;
	ORGCloudShadows();

	/** @brief After Setup. Every layer is a twin in graph mode, so a failure is fatal. */
	void SetupLayerOps();

	std::shared_ptr<Impl> impl;
	std::shared_ptr<LayerOps> layerOps;
};

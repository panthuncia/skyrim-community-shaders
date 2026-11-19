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
* @brief Cloud Shadows' occlusion copy and layer operations on the render graph, with CS_ORG_CLOUD_SHADOWS=1.
*
* Each cloud layer drawn into a reflection face also writes its occlusion into one of 32 layer cubemaps. At the first
* reflections prepass of a frame the last layer is copied into the occlusion cubemap every reader samples. A copy has no
* indirect form, so this is a gated compute copy and the recording stays the same.
* The clears and copies of layer faces between draws are queued as layer operations and run at the next anchor as a gather
* pass then a scatter pass. Before any D3D11 use of the layers, whatever is queued is done on D3D11 first.
* Faces the engine draws keep their operations on D3D11. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGCloudShadows
{
public:
	/** @brief The occlusion cubemap's published id. */
	static constexpr std::string_view kOcclusionId = "CloudShadows::Occlusion";

	static bool Requested();

	/** @brief Call in PostPostLoad, before the render graph initializes. */
	static void Reserve();

	static ORGCloudShadows& Get();

	/** @brief Every graph user takes a layer through this, so one resource serves them all. Null without layers or an active runtime. */
	static FrameAnchors::Texture ImportLayer(int a_layer);

	/** @brief Empty when the layer has no twin or the runtime is not active. */
	static std::string PublishedLayerId(int a_layer);

	/**
	* @brief Makes a persistent, exported, published cube twin for a layer. It lives as long as the game, since D3D11 keeps drawing into its export.
	* @return A wrapper with an SRV for Cloud Shadows, or null to make a D3D11 texture.
	*/
	static Texture2D* MakeLayer(int a_layer, const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_SHADER_RESOURCE_VIEW_DESC& a_srvDesc, const char* a_name);

	/** @brief Call at SetupResources after the layers exist. @return False keeps the copy on D3D11. */
	bool Setup();

	/** @brief With the runtime still up. */
	bool IsActive() const;

	/** @brief The ReflectionsPrepass anchor runs it right after the features' reflection prepasses. @return False when the copy stays on D3D11. */
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
	* @brief Render thread. False means the caller does it on D3D11 after doing what TakeLayerOps returns.
	* That happens when the point is D3D11 or cannot run now, another point's ops are still queued, or the scratch array is full.
	*/
	bool QueueLayerOp(LayerOpPoint a_point, LayerOp a_op);

	/** @brief In call order, for the caller to do on D3D11. Render thread. */
	std::vector<LayerOp> TakeLayerOps();

	~ORGCloudShadows();

	struct Impl;

	struct LayerOps;

private:
	ORGCloudShadows() = default;

	/** @brief For good. */
	void Stop();

	/** @brief Only when every layer is a twin. False keeps them on D3D11. */
	bool SetupLayerOps();

	std::shared_ptr<Impl> impl;
	std::shared_ptr<LayerOps> layerOps;
};

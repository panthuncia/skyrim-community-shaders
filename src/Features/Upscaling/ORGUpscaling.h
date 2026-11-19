#pragma once

#include "RenderGraph/Engine/Native/Native.h"

#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <span>
#include <string>

/**
* @brief Upscaling's own passes on the render graph, with CS_ORG_UPSCALING=1.
*
* Streamline's evaluate is the Upscale anchor's first pass. It records the SDK's tags and evaluate on the pass's own command list.
* The upscaled output is exported to D3D11, and a gated draw copies it into kMAIN. The depth upscale runs at the same anchor.
* A copy has no per-run switch, so the client is rebuilt when the depth upscale starts or stops.
* Frame generation still runs in the DXVK interop command ring. Its hudless colour is copied at the FrameGeneration anchor.
* On any failure the feature returns to D3D11 once the ring is drained. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGUpscaling
{
public:
	/** @brief Textures the graph holds instead of D3D11 while its path runs. */
	enum class Target : std::uint8_t
	{
		Upscaled,  // kMAIN's description, SRV and UAV
		Hudless,   // SRV and RTV
	};

	static bool Requested();

	static ORGUpscaling& Get();

	/** @brief PostPostLoad, when requested. */
	static void Reserve();

	/**
	* @brief SetupResources, with the runtime active. Clients register at the first frame that queues their work.
	* @return False keeps D3D11, logged.
	*/
	bool Setup();

	/** @brief Retries a failed Setup. Built programs reload through the pipeline registry. */
	void ClearShaderCache();

	/** @brief Set up, not failed, and the runtime still up. */
	bool IsActive() const;

	/** @brief Whether the evaluate is the post stage's pass. */
	bool EvaluatesInStage() const;

	/** @brief The textures it holds must then go back to D3D11. */
	bool Failed() const;

	/**
	* @brief Call after a failure with the command ring drained. The feature stays on D3D11 from then on.
	* @return Whether it held the upscaled texture, which the caller must remake in D3D11.
	*/
	bool ReleaseAfterFailure();

	/** @brief False means the caller makes the D3D11 texture. */
	bool CreateTexture(Target a_target, const D3D11_TEXTURE2D_DESC& a_desc);

	/** @brief The caller must have drained the command ring. */
	void DestroyTexture(Target a_target);

	bool Holds(Target a_target) const;

	/** @brief The graph's D3D11 export while the graph holds it, else a_d3d11. */
	ID3D11Texture2D* Texture(Target a_target, ID3D11Texture2D* a_d3d11) const;

	/** @brief Chosen as Texture chooses. */
	const D3D11_TEXTURE2D_DESC* Desc(Target a_target, const D3D11_TEXTURE2D_DESC* a_d3d11) const;

	/**
	* @brief Records the SDK's commands on the native list, a VkCommandBuffer or ID3D12GraphicsCommandList.
	* Images in order: colour in (kMAIN), colour out, depth, motion vectors. False when the SDK refused.
	*/
	using EvaluateRecord = std::function<bool(void* a_nativeList, std::span<const RenderGraph::Engine::Native::ExternalImageInfo> a_images)>;

	/**
	* @brief a_record runs once, in the evaluate pass's Record, on whichever thread records it.
	* False when the graph path does not run, holds no upscaled texture, or the inputs cannot be imported.
	*/
	bool QueueEvaluate(std::string a_label, EvaluateRecord a_record);

	/** @brief After an evaluate that wrote the upscaled texture. False means copy with D3D11. */
	bool QueueCopyToMain();

	/** @brief @return False when the engine's targets cannot be imported. Then draw with D3D11. */
	bool QueueDepthUpscale(float2 a_jitter, float a_useWideKernel);

	/** @brief In PerformUpscaling. Runs the anchor when something was queued. */
	void RunUpscale();

	/** @brief @return False when nothing was drawn. */
	bool CopyHudless(ID3D11ShaderResourceView* a_source);

	~ORGUpscaling();

	struct Impl;

private:
	ORGUpscaling() = default;

	std::shared_ptr<Impl> impl;
	bool setupFailed = false;  // the runtime was there but programs or resources failed
};

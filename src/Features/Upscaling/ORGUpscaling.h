#pragma once

#include "RenderGraph/Engine/Native/Native.h"

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

/**
* @brief Upscaling's own passes on the render graph, with CS_ORG_UPSCALING=1.
*
* Streamline's evaluate is the Upscale anchor's first pass. It records the SDK's tags and evaluate on the pass's own command list.
* The upscaled output is exported to D3D11, and a gated draw copies it into kMAIN. The depth upscale runs at the same anchor.
* Frame generation's hudless colour is copied at the UI stage's FrameGeneration step, and its inputs (FSR-FG's prepare) are a pass
* of the same step after the copy. Plain D3D11 mode keeps them in the DXVK interop command ring.
*
* Its textures and clients are declared on variables (frame revision plan 6.14), so they switch in with the revision that changes
* them. The upscaled texture follows DisplaySize, UpscaleMethod and frame generation, and is one texel when nothing evaluates into
* it. The hudless colour has one texture per format (kHudlessFormats): the one HdrEnabled and the presenter's format choose is at
* the display size, the others one texel. The Upscale client follows the display size, the method and the quality (the depth
* upscale runs only below native), the hudless client the chosen format.
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

	/** @brief The hudless colour's formats, each with its own texture. */
	static constexpr std::array<DXGI_FORMAT, 3> kHudlessFormats{ DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM };

	/**
	* @brief Which of kHudlessFormats the hudless colour takes. HDR always lands in 10 bits, the HDR10 back buffer FFX subtracts it
	* from. SDR takes the presenter's format. None while the presenter is in the other encoding, or has no format FFX can use.
	*/
	static std::optional<std::uint32_t> HudlessFormatIndex(bool a_hdr, DXGI_FORMAT a_presenterFormat, bool a_presenterHdr10)
	{
		if (a_hdr)
			return 2u;
		if (a_presenterHdr10)
			return std::nullopt;
		for (std::uint32_t i = 0; i < kHudlessFormats.size(); ++i)
			if (kHudlessFormats[i] == a_presenterFormat)
				return i;
		return std::nullopt;
	}

	static ORGUpscaling& Get();

	/** @brief PostPostLoad, when requested. */
	static void Reserve();

	/**
	* @brief SetupResources, with the runtime active. Declares the textures and clients: the first frame revision registers them.
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

	/** @brief Whether the graph holds the target, so the caller makes no D3D11 texture for it. */
	bool Holds(Target a_target) const;

	/**
	* @brief The graph's D3D11 export while the graph holds the target, else a_d3d11. The hudless colour is the adopted revision's
	* format's texture, null while that revision has none.
	*/
	ID3D11Texture2D* Texture(Target a_target, ID3D11Texture2D* a_d3d11) const;

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

	/** @brief Whether frame generation's inputs are the UI stage's pass: the graph holds the hudless and a stage holds its step. */
	bool RecordsFrameGenInputs() const;

	/**
	* @brief Runs frame generation's inputs pass now, after the hudless copy. a_record runs once, in the pass's Record, on whichever
	* thread records the UI stage. Images in order: depth (kMAIN's, or kMAIN_COPY's when a_depthCopy), motion vectors, hudless.
	* False when the pass cannot run this frame: no graph hudless, an engine target missing, or its build not switched in yet.
	*/
	bool QueueFrameGenInputs(std::string a_label, EvaluateRecord a_record, bool a_depthCopy);

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

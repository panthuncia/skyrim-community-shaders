#pragma once

#include <d3d11.h>
#include <memory>

/**
 * @brief Upscaling's own passes on the render graph (CS_ORG_UPSCALING=1). The vendor upscalers do not move: Streamline's FSR,
 * XeSS and DLSS evaluates, and frame generation, still run in the DXVK interop command ring (Upscaling/DXVKInterop.cpp).
 *
 * - The upscaler's output is a graph texture exported to D3D11 (SRV and UAV), because Streamline still writes it through its
 *   D3D11 image. At the Upscale anchor, which runs after the evaluate, a copy pass puts it into kMAIN (the D3D11 path's
 *   CopyResource).
 * - The depth upscale at the same anchor: kMAIN's depth into kMAIN_COPY and the refraction normals into their copy, then
 *   DepthRefractionUpscalePS into the refraction normals, kSAO_CAMERAZ and kMAIN's depth, then the underwater mask's copy and
 *   UnderwaterMaskUpscalePS at half size. The jitter constants come from a latch block, the shared constant buffers from
 *   SharedConstants.
 * - Frame generation's hudless colour is a graph texture exported to D3D11 (SRV and RTV), which Streamline tags or reads. At the
 *   FrameGeneration anchor CopyHudlessPS draws the framebuffer, or HDR's clean composite, into it.
 *
 * Ordering against the ring: the ring's buffer is submitted after DXVK's command list in flight, and ends with a full memory
 * barrier, so the Upscale epoch that follows in DXVK's next list sees the evaluate's output. Before an evaluate or a frame
 * generation call, a late anchor's epoch is separated from it (Upscaling::SeparateFromGraphEpochs), as before.
 *
 * Every pass checks per run whether it has work (the client's active publishes it), so a kept recording does nothing on a frame
 * where the D3D11 path would have skipped that work. On any failure the feature returns to D3D11: the graph textures are released
 * once the ring is drained and the D3D11 ones are made again. Render thread throughout.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGUpscaling
{
public:
	/** @brief Whether CS_ORG_UPSCALING=1 is set. */
	static bool Requested();

	static ORGUpscaling& Get();

	/** @brief PostPostLoad, when requested: reserves the Upscale and FrameGeneration anchors. */
	static void Reserve();

	/** @brief SetupResources, with the runtime active: makes the programs, the sampler and the latch. Logged, D3D11 on failure. */
	void Setup();

	/**
	 * @brief Upscaling::ClearShaderCache: a Setup that failed is tried again. Programs that built reload through the pipeline
	 * registry, so nothing else is dropped.
	 */
	void ClearShaderCache();

	/** @brief Whether the graph path runs: set up and not failed. */
	bool Running() const;

	/** @brief Whether the graph failed and the textures it holds must go back to D3D11 (ReleaseAfterFailure). */
	bool Failed() const;

	/**
	 * @brief After a failure, with the command ring drained: drops the clients, the graph textures and the programs. The feature is
	 * on D3D11 from then on. @return Whether it held the upscaled texture (the caller makes the D3D11 one again).
	 */
	bool ReleaseAfterFailure();

	/** @brief The upscaler's output as an exported graph texture (a_desc: kMAIN's, SRV and UAV). False: the D3D11 texture is used. */
	bool CreateUpscaledTexture(const D3D11_TEXTURE2D_DESC& a_desc);
	/** @brief Releases the upscaled texture. The caller has drained the command ring. */
	void DestroyUpscaledTexture();
	/** @brief The upscaled texture's D3D11 export, or null when the graph holds none. */
	ID3D11Texture2D* UpscaledTexture() const;

	/** @brief Frame generation's hudless colour as an exported graph texture (SRV and RTV). False: the D3D11 texture is used. */
	bool CreateHudlessTexture(const D3D11_TEXTURE2D_DESC& a_desc);
	/** @brief Releases the hudless texture. The caller has drained the command ring. */
	void DestroyHudlessTexture();
	/** @brief The hudless texture's D3D11 export, or null when the graph holds none. */
	ID3D11Texture2D* HudlessTexture() const;
	/** @brief The hudless texture's description, or null when the graph holds none. */
	const D3D11_TEXTURE2D_DESC* HudlessDesc() const;

	/** @brief After an evaluate that wrote the upscaled texture: the Upscale anchor copies it into kMAIN. False: copy with D3D11. */
	bool QueueCopyToMain();

	/**
	 * @brief UpscaleDepth's work at the Upscale anchor, with this frame's jitter and kernel choice (Upscaling::JitterCB).
	 * @return False when the engine's targets cannot be imported: draw with D3D11.
	 */
	bool QueueDepthUpscale(float2 a_jitter, float a_useWideKernel);

	/** @brief In PerformUpscaling, after the evaluate and UpscaleDepth: runs the Upscale anchor when something was queued. */
	void RunUpscale();

	/**
	 * @brief Draws a_source into the hudless texture at the FrameGeneration anchor. @return False when nothing was drawn (the graph
	 * failed, or the source cannot be imported).
	 */
	bool CopyHudless(ID3D11ShaderResourceView* a_source);

	~ORGUpscaling();

	struct Impl;

private:
	ORGUpscaling() = default;

	std::shared_ptr<Impl> impl;
	bool setupFailed = false;  // the last Setup found the runtime but could not make its programs or resources
};

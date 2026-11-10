#pragma once

#include <cstdint>
#include <d3d11.h>
#include <memory>

/**
 * @brief Upscaling's own passes on the render graph (CS_ORG_UPSCALING=1). The vendor upscalers do not move: Streamline's FSR,
 * XeSS and DLSS evaluates, and frame generation, still run in the DXVK interop command ring (Upscaling/DXVKInterop.cpp).
 *
 * - The upscaler's output is a graph texture exported to D3D11 (SRV and UAV), because Streamline still writes it through its
 *   D3D11 image. At the Upscale anchor, which runs after the evaluate, CopyHudlessPS draws it into kMAIN (the D3D11 path's
 *   CopyResource). It is a draw so that a run gate can skip it on frames the evaluate wrote nothing.
 * - The depth upscale at the same anchor: kMAIN's depth into kMAIN_COPY and the refraction normals into their copy, then
 *   DepthRefractionUpscalePS into the refraction normals, kSAO_CAMERAZ and kMAIN's depth, then the underwater mask's copy and
 *   UnderwaterMaskUpscalePS at half size. The jitter constants come from a latch block, the shared constant buffers from
 *   SharedConstants. A copy has no per-run switch, so these passes are in the anchor's graph only while the depth upscale runs:
 *   the client is built again when it starts or stops.
 * - Frame generation's hudless colour is a graph texture exported to D3D11 (SRV and RTV), which Streamline tags or reads. At the
 *   FrameGeneration anchor CopyHudlessPS draws the framebuffer, or HDR's clean composite, into it.
 *
 * The draws go through run gates (RenderGraph/RunGate.h): each client's commit opens the draws this frame queued, so a kept
 * recording does nothing where the D3D11 path would have skipped that work.
 *
 * Ordering against the ring: the ring's buffer is submitted after DXVK's command list in flight, and ends with a full memory
 * barrier, so the Upscale epoch that follows in DXVK's next list sees the evaluate's output. Before an evaluate or a frame
 * generation call, a late anchor's epoch is separated from it (Upscaling::SeparateFromGraphEpochs), as before.
 *
 * On any failure the feature returns to D3D11: once the ring is drained the graph textures are released and the D3D11 ones are
 * made again (Failed, ReleaseAfterFailure). Render thread throughout.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGUpscaling
{
public:
	/** @brief Upscaling's own textures, which the graph holds instead of D3D11 while its path runs. */
	enum class Target : std::uint8_t
	{
		Upscaled,  // the upscaler's output: kMAIN's description, SRV and UAV
		Hudless,   // frame generation's hudless colour: SRV and RTV
	};

	/** @brief Whether CS_ORG_UPSCALING=1 is set. */
	static bool Requested();

	/** @brief The one instance, for the session. */
	static ORGUpscaling& Get();

	/** @brief PostPostLoad, when requested: reserves the Upscale and FrameGeneration anchors. */
	static void Reserve();

	/**
	 * @brief SetupResources, with the runtime active: makes the programs, the sampler, the latch and the run gates. The clients
	 * register at the first frame that queues their work, since what they hold (the textures, the engine targets) is known then.
	 * @return Whether the graph path runs. False keeps D3D11 (logged).
	 */
	bool Setup();

	/**
	 * @brief Upscaling::ClearShaderCache: a Setup that failed is tried again. Programs that built reload through the pipeline
	 * registry, so nothing else is dropped.
	 */
	void ClearShaderCache();

	/** @brief Whether the graph path runs: set up, not failed, and the runtime still up. */
	bool IsActive() const;

	/** @brief Whether the graph failed and the textures it holds must go back to D3D11 (ReleaseAfterFailure). */
	bool Failed() const;

	/**
	 * @brief After a failure, with the command ring drained: drops the clients, the graph textures and the programs. The feature is
	 * on D3D11 from then on. @return Whether it held the upscaled texture (the caller makes the D3D11 one again).
	 */
	bool ReleaseAfterFailure();

	/** @brief a_target as an exported graph texture (a_desc: the D3D11 description). False: the caller makes the D3D11 texture. */
	bool CreateTexture(Target a_target, const D3D11_TEXTURE2D_DESC& a_desc);

	/** @brief Releases a_target's graph texture, if the graph holds it. The caller has drained the command ring. */
	void DestroyTexture(Target a_target);

	/** @brief Whether the graph holds a_target. */
	bool Holds(Target a_target) const;

	/**
	 * @brief The texture Upscaling uses for a_target: the graph's D3D11 export while the graph holds it, else a_d3d11 (Upscaling's
	 * own D3D11 texture, or null).
	 */
	ID3D11Texture2D* Texture(Target a_target, ID3D11Texture2D* a_d3d11) const;

	/** @brief a_target's description, chosen as Texture chooses: the graph texture's, else a_d3d11. */
	const D3D11_TEXTURE2D_DESC* Desc(Target a_target, const D3D11_TEXTURE2D_DESC* a_d3d11) const;

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

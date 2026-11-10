#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>

/**
 * @brief Screen Space GI on the render graph (CS_ORG_SCREEN_SPACE_GI=1).
 *
 * The D3D11 chain as graph compute passes at the DeferredPasses anchor, which runs right before the D3D11 SSGI would: prefilter
 * depths, radiance and disocclusion, prefilter radiance, prefilter normals, GI, then blur and upsample when they are on. The
 * passes are AnchorPasses dispatches. Each dispatches through its entry of the client's run gate, which the commit opens with
 * this frame's group counts, so a kept recording follows dynamic resolution and does no work on a run that did not open it.
 * SSGI's per-frame constants (SSGICB) are the latch entry; the shaders also read the shared constant buffers.
 *
 * The D3D11 path swaps ping-pong pairs with function-static indices. Here each pair is split by lifetime: the texture the next
 * frame reads is a History texture (AO, IlY, IlCoCg, specular, accumulated frames, previous geometry), the scratch texture a
 * Transient one the graph may alias, as are the prefiltered depth, normal and radiance chains. The composite's inputs (AO, IlY,
 * IlCoCg, specular: the History textures at full resolution, the upsampled scratch textures otherwise) are exported to D3D11,
 * so they are not aliased while the D3D11 composite reads them.
 *
 * A history reset (loading screen, shader recompile, new textures) clears the History textures: one-shot passes of a second
 * client, armed again by FrameAnchors::Rearm.
 *
 * On any failure the feature returns to its D3D11 path. Render thread throughout.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGScreenSpaceGI
{
public:
	/** @brief Whether CS_ORG_SCREEN_SPACE_GI=1 is set. */
	static bool Requested();

	static ORGScreenSpaceGI& Get();

	/** @brief Reserves the DeferredPasses anchor when requested (the feature's PostPostLoad, before the graph initializes). */
	static void Reserve();

	/**
	 * @brief Makes the samplers, programs and textures, sized as a_main (kMAIN's description). With the runtime active (the
	 * feature's SetupResources). The anchor client is registered at the first Prepass, which imports the engine targets.
	 * @param a_onFailed Called once if the graph path stops later; the feature then draws with D3D11.
	 * @return Whether SSGI runs on the render graph; false, logged, keeps it on D3D11.
	 */
	bool Setup(const D3D11_TEXTURE2D_DESC& a_main, std::function<void()> a_onFailed);

	/**
	 * @brief At the feature's Prepass, before the DeferredPasses anchor of the frame: makes the programs and textures again after
	 * a settings change or a recompile, imports the inputs, arms a history reset, and registers the anchor clients when any of
	 * these changed. Any failure returns the feature to D3D11.
	 */
	void Prepass();

	/** @brief Whether the graph path owns SSGI: set up, not failed, and the runtime still up. The D3D11 textures are then not made. */
	bool IsActive() const;

	/** @brief The composite's inputs, as D3D11 SRVs (t10-t13), when the graph path runs and SSGI is enabled; nulls otherwise. */
	struct Outputs
	{
		ID3D11ShaderResourceView* ao = nullptr;
		ID3D11ShaderResourceView* y = nullptr;
		ID3D11ShaderResourceView* coCg = nullptr;
		ID3D11ShaderResourceView* specular = nullptr;
	};
	Outputs GetOutputs() const;

	~ORGScreenSpaceGI();

private:
	ORGScreenSpaceGI() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};

#pragma once

#include <d3d11.h>
#include <memory>

namespace org
{
	class Resource;
}

/**
 * @brief Screen Space GI on the render graph (CS_ORG_SCREEN_SPACE_GI=1).
 *
 * The D3D11 chain as graph compute passes at the DeferredPasses anchor, which runs right before the D3D11 SSGI would: prefilter
 * depths, radiance and disocclusion, prefilter radiance, prefilter normals, GI, then blur and upsample when they are on. Each pass
 * dispatches indirectly, its group counts in the frame's latch region beside SSGI's per-frame constants (SSGICB), so a kept
 * recording follows dynamic resolution. The shaders also read the shared constant buffers (SharedConstants).
 *
 * The D3D11 path swaps ping-pong pairs with function-static indices. Here each pair is split by lifetime: the texture the next
 * frame reads is a History texture (AO, IlY, IlCoCg, specular, accumulated frames, previous geometry), the scratch texture a
 * Transient one the graph may alias, as are the prefiltered depth, normal and radiance chains. The composite's inputs (AO, IlY,
 * IlCoCg, specular: the History textures at full resolution, the upsampled scratch textures otherwise) are exported to D3D11,
 * so they are not aliased while the D3D11 composite reads them.
 *
 * A history reset (loading screen, shader recompile, new textures) adds clears of the History textures to one build.
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
	 * feature's SetupResources). Logged and left on D3D11 on failure.
	 */
	void Setup(const D3D11_TEXTURE2D_DESC& a_main);

	/**
	 * @brief At the feature's Prepass, before the DeferredPasses anchor of the frame: makes the programs and textures again after
	 * a settings change or a recompile, imports the inputs, queues a history reset, and registers the anchor client the first
	 * time (the shared constants exist by then). Any failure leaves the frame to D3D11 (Draw).
	 */
	void Prepass();

	/**
	 * @brief At the feature's DrawSSGI, after the anchor ran: whether the graph path owns SSGI this frame. False when it is off or
	 * just failed (then unregistered, and the feature draws with D3D11 from now on).
	 */
	bool Draw();

	/** @brief Whether the graph path is set up and has not failed: the D3D11 textures are then not made. */
	bool Running() const;

	/** @brief The composite's inputs, as D3D11 SRVs (t10-t13), when the graph path runs and SSGI is enabled; nulls otherwise. */
	struct Outputs
	{
		ID3D11ShaderResourceView* ao = nullptr;
		ID3D11ShaderResourceView* y = nullptr;
		ID3D11ShaderResourceView* coCg = nullptr;
		ID3D11ShaderResourceView* specular = nullptr;
	};
	Outputs GetOutputs() const;

	/**
	 * @brief The same textures as graph resources, for a graph composite: registered by name with the DeferredPasses graph
	 * ("SSGI::AO", "SSGI::IlY", "SSGI::IlCoCg", "SSGI::GiSpecular", or their "...Scratch" twins when SSGI upsamples). Null when
	 * the graph path does not run. They change when the settings rebuild the textures: read them at each build.
	 */
	struct GraphOutputs
	{
		std::shared_ptr<org::Resource> ao, y, coCg, specular;
	};
	GraphOutputs GetGraphOutputs() const;

	~ORGScreenSpaceGI();

private:
	ORGScreenSpaceGI() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};

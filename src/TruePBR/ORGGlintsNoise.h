#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

/**
 * @brief TruePBR's glints noise as a render-graph pass (CS_ORG_GLINTS=1).
 *
 * The noise is a 128x128 RGBA32F texture that never changes: noisegen.cs.hlsl fills it once. On this path a one-shot compute pass
 * (FrameAnchors::PassDesc::once) at the EarlyPrepass anchor fills it at the anchor's first run after Setup. That anchor runs
 * before every reader in the frame (the depth prepass, the water reflections, the main pass). The client is active until the
 * executed signal reports the run with the fill; the anchors then build the graph again without the pass.
 *
 * Resources:
 * - The noise: a persistent graph texture named "TruePBR::GlintsNoise". It exports a D3D11 SRV, which TruePBR binds at t20 for the
 *   engine's Lighting pixel shaders until those draws are ported. Until the fill ran TruePBR binds nothing there. It is also published under its name, and Drawcall Limit Fix's
 *   colour passes read it from there (Integrations/TruePBR), ordered after the fill.
 *
 * The render graph has no device reset. When the graph fails, the client is dropped and TruePBR goes back to D3D11, where the
 * noise is made again on its next Prepass.
 *
 * On any failure (no runtime, a shader that does not compile, a texture that cannot be made, the graph stopping) it stops for
 * good and Active() turns false.
 *
 * Everything here runs on the render thread. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGGlintsNoise
{
public:
	/** @brief The noise texture's published id (PublishedResources), its twin's name. */
	static constexpr std::string_view kNoiseId = "TruePBR::GlintsNoise";

	/** @brief Whether CS_ORG_GLINTS=1 is set. */
	static bool Requested();

	/** @brief Reserves the EarlyPrepass anchor when requested. Call before the render graph initializes (PostPostLoad). */
	static void Reserve();

	/** @brief The one instance. */
	static ORGGlintsNoise& Get();

	/**
	 * @brief After the render graph initializes (TruePBR::PostSetupResources), when requested: makes the program and the texture,
	 * registers the client with its one-shot fill and publishes the texture. The device is the graph host's.
	 * @param a_onFailed Called when the graph fails later and the noise returns to D3D11.
	 * @return Whether the noise is on the render graph; false keeps the D3D11 path.
	 */
	bool Setup(std::function<void()> a_onFailed = {});

	/** @brief Whether the noise is the graph's, its client is registered and the runtime is still up. */
	bool Active() const;

	/** @brief Whether a run with the fill was submitted (ClientDesc::executed), so later D3D11 work and graph readers see it filled. */
	bool Filled() const;

	/** @brief The noise's exported view (t20); null when inactive or not filled yet. */
	ID3D11ShaderResourceView* NoiseSRV() const;

	/**
	 * @brief TruePBR::ClearShaderCache: builds the fill program again and fills the noise again. After a failed compile, sets the
	 * path up again.
	 */
	void ClearShaderCache();

	~ORGGlintsNoise();

	/** @brief The resources and fill state the pass and the client share (ORGGlintsNoise.cpp). */
	struct Impl;

private:
	ORGGlintsNoise() = default;

	/** @brief Registers (or replaces) the client with a_impl's program: its fill is pending again. False on failure. */
	bool RegisterClient(const std::shared_ptr<Impl>& a_impl);

	/** @brief Stops the ORG path for good. */
	void Fail();

	std::shared_ptr<Impl> impl;
	std::function<void()> onFailed;
	bool failed = false;
	bool programFailed = false;  // the failure was the fill program's compile, which a shader reload may fix
};

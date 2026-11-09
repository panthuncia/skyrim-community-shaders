#pragma once

#include <d3d11.h>
#include <memory>

/**
 * @brief TruePBR's glints noise as a render-graph pass (CS_ORG_GLINTS=1).
 *
 * The noise is a 128x128 RGBA32F texture that never changes: noisegen.cs.hlsl fills it once. On this path a compute pass at the
 * EarlyPrepass anchor fills it, in the first frame after TruePBR's EarlyPrepass set it up. That anchor runs before every reader in
 * the frame (the depth prepass, the water reflections, the main pass). The client is active only until that epoch is submitted.
 * The next frame builds the graph again without the pass.
 *
 * Resources:
 * - The noise: a persistent graph texture named "TruePBR::GlintsNoise". It exports a D3D11 SRV, which TruePBR binds at t20 for the
 *   engine's Lighting pixel shaders until those draws are ported. It is also published under its name, and Drawcall Limit Fix's
 *   colour passes read it from there (Integrations/TruePBR), ordered after the fill.
 *
 * The render graph has no device reset. When the graph fails, the client is dropped and TruePBR goes back to D3D11, where the
 * noise is made again on its next Prepass. A new Setup (never after a failure) fills the new texture again.
 *
 * On any failure (no runtime, a shader that does not compile, a texture that cannot be made, the graph stopping) it stops for
 * good and Active() turns false.
 *
 * Everything here runs on the render thread. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGGlintsNoise
{
public:
	/** @brief Whether CS_ORG_GLINTS=1 is set. */
	static bool Requested();

	/** @brief Reserves the EarlyPrepass anchor when requested. Call before the render graph initializes (PostPostLoad). */
	static void Reserve();

	static ORGGlintsNoise& Get();

	/**
	 * @brief TruePBR's EarlyPrepass: makes the program, the texture and the client the first time, and drops the fill pass once its
	 * epoch was submitted.
	 * @return Whether the noise is on the render graph.
	 */
	bool Update();

	/** @brief Whether the noise is the graph's and its client is registered. */
	bool Active() const;

	/** @brief Whether the fill's epoch was submitted, so later D3D11 work and graph readers see the filled texture. */
	bool Filled() const;

	/** @brief The noise's exported view (t20); null when inactive. */
	ID3D11ShaderResourceView* NoiseSRV() const;

	~ORGGlintsNoise();

	/** @brief The resources and fill state the pass and the client share (ORGGlintsNoise.cpp). */
	struct Impl;

private:
	ORGGlintsNoise() = default;

	/** @brief Makes the program and texture, registers the client and publishes the texture. False, logged, on failure. */
	bool Setup();

	/** @brief Stops the ORG path for good. */
	void Fail();

	std::shared_ptr<Impl> impl;
	bool failed = false;
};

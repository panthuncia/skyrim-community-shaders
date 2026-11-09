#pragma once

#include <d3d11.h>
#include <memory>

/**
 * @brief Screen Space Shadows on the render graph (CS_ORG_SCREEN_SPACE_SHADOWS=1).
 *
 * Two passes at the Prepass anchor: a clear of the shadow texture to white, then one raymarch pass with a dispatch for each
 * of Bend's dispatch-list entries. The dispatch list changes every frame, while the recording is kept and submitted again, so
 * the passes record the list's capacity: each dispatch takes its constants and its thread-group counts (an indirect dispatch)
 * from the execution's region of a latch block, which the client's commit writes. Unused entries dispatch no groups.
 *
 * Depth is the engine's kPOST_ZPREPASS_COPY (FrameAnchors::EngineDepth), or Terrain Blending's blended depth while that is on
 * (FrameAnchors::D3D11Texture). The output is a graph texture exported to D3D11, which the engine's pixel shaders read at t45.
 *
 * On any failure the feature returns to its D3D11 path. Render thread throughout.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGScreenSpaceShadows
{
public:
	/** @brief Whether CS_ORG_SCREEN_SPACE_SHADOWS=1 is set. */
	static bool Requested();

	static ORGScreenSpaceShadows& Get();

	/**
	 * @brief Makes the program, the shadow texture (a_desc: the D3D11 texture's description) and the latch, and registers the
	 * anchor client. With the runtime active (the feature's SetupResources). Logged and left on D3D11 on failure.
	 */
	void Setup(const D3D11_TEXTURE2D_DESC& a_desc);

	/**
	 * @brief At the feature's Prepass: keeps the program and the depth input current, the passes then running in the anchor's
	 * epoch. @return The shadow texture's SRV for the engine's t45, or null when the feature uses its D3D11 path.
	 */
	ID3D11ShaderResourceView* Prepass();

	~ORGScreenSpaceShadows();

private:
	ORGScreenSpaceShadows() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};

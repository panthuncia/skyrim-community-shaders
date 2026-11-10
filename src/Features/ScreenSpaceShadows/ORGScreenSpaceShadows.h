#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

/**
 * @brief Screen Space Shadows on the render graph (CS_ORG_SCREEN_SPACE_SHADOWS=1).
 *
 * Two passes at the Prepass anchor: a clear of the shadow texture to white, then one raymarch pass with a dispatch for each
 * of Bend's dispatch-list entries. The dispatch list changes every frame, while the recording is kept and submitted again, so
 * the raymarch records the list's capacity. Each dispatch reads its constants from the execution's region of a latch block and
 * its thread-group counts from a run gate entry, both written by the client's commit. Unused entries stay closed and dispatch
 * no groups.
 *
 * Depth is the engine's kPOST_ZPREPASS_COPY (FrameAnchors::EngineDepth), or Terrain Blending's blended depth while that is on:
 * by its published id (ORGTerrainBlending::kBlendedDepthId) on Terrain Blending's graph path, else its D3D11 texture
 * (FrameAnchors::D3D11Texture). The output is a graph texture exported to D3D11, which the engine's pixel shaders read at t45. It
 * is also published (PublishedResources, kShadowTextureId): Drawcall Limit Fix's colour passes read it through the graph.
 *
 * On any failure the feature returns to its D3D11 path. Render thread throughout.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGScreenSpaceShadows
{
public:
	/** @brief The shadow texture's published id (PublishedResources), its twin's name. */
	static constexpr std::string_view kShadowTextureId = "ScreenSpaceShadows::ShadowTexture";

	/** @brief Whether CS_ORG_SCREEN_SPACE_SHADOWS=1 is set. */
	static bool Requested();

	/** @brief Reserves the Prepass anchor when requested. Call before the render graph initializes (PostPostLoad). */
	static void Reserve();

	static ORGScreenSpaceShadows& Get();

	/**
	 * @brief Makes the program, the shadow texture (a_desc: the D3D11 texture's description), the latch and the run gate, and
	 * registers the anchor client. With the runtime active (the feature's SetupResources).
	 * @param a_onFailed Called once if the graph fails later; the feature returns to D3D11.
	 * @return Whether the shadows run on the render graph; false keeps the D3D11 path.
	 */
	bool Setup(const D3D11_TEXTURE2D_DESC& a_desc, std::function<void()> a_onFailed);

	/** @brief Whether the shadows run on the render graph: the client is registered and the runtime is still up. */
	bool IsActive() const;

	/**
	 * @brief At the feature's Prepass: keeps the program and the depth input current, the passes then running in the anchor's
	 * epoch. @return The shadow texture's SRV for the engine's t45, or null when the feature uses its D3D11 path.
	 */
	ID3D11ShaderResourceView* Prepass();

	/** @brief The shadow texture's exported SRV while the ORG path runs; null on the D3D11 path. */
	ID3D11ShaderResourceView* ShadowSRV() const;

	~ORGScreenSpaceShadows();

private:
	ORGScreenSpaceShadows() = default;

	/** @brief Leaves the ORG path: the client unregistered, the texture withdrawn, the state dropped. */
	void Stop();

	struct Impl;
	std::unique_ptr<Impl> impl;
};

#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>

/**
 * @brief Subsurface Scattering on the render graph (CS_ORG_SUBSURFACE_SCATTERING=1).
 *
 * The passes run at the DeferredPasses anchor, where the D3D11 path runs them: before the deferred composite reads the main
 * target. Separable mode is three passes: the diffuse extraction (main target to the albedo-free copy), the horizontal blur (to
 * a temporary texture) and the vertical blur (back into the main target). Burley mode clears the albedo-free copy, extracts the
 * masked pixels into it and blurs them back into the main target.
 *
 * The main target, the GBuffer's masks, albedo and normals, and the final depth copy are engine targets
 * (FrameAnchors::EngineTarget, EngineDepth), so the D3D11 composite and everything after it read the result as before. The two
 * intermediate textures are graph textures that live within the frame, so the graph may alias them.
 *
 * The constants change every frame while the recording is kept: the client's commit writes them into the execution's region
 * of a latch block, and opens the passes' run gate on a frame with skin. A frame without skin dispatches no groups. The shared
 * constant buffers come from SharedConstants.
 *
 * On any failure the feature returns to its D3D11 path. Render thread throughout.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGSubsurfaceScattering
{
public:
	/** @brief Whether CS_ORG_SUBSURFACE_SCATTERING=1 is set. */
	static bool Requested();

	/** @brief Reserves the DeferredPasses anchor when requested. Call before the render graph initializes (PostPostLoad). */
	static void Reserve();

	static ORGSubsurfaceScattering& Get();

	/**
	 * @brief At the feature's SetupResources, with the runtime active: makes the programs, the intermediate textures (a_desc: the
	 * main target's description with SRV and UAV binds), the latch and the run gate, and registers the anchor client. The client
	 * joins the graphs once CS's shared constants exist; the D3D11 path draws until then.
	 * @param a_onFailed Called once if the graph fails later; the feature returns to D3D11.
	 * @return Whether the passes run on the render graph; false keeps the D3D11 path.
	 */
	bool Setup(const D3D11_TEXTURE2D_DESC& a_desc, std::function<void()> a_onFailed);

	/** @brief Whether the render-graph path is set up and the runtime is still up. */
	bool IsActive() const;

	/**
	 * @brief At the feature's DrawSSS, after the DeferredPasses anchor ran: keeps the passes current for the next frame (the mode,
	 * the programs after a shader cache clear, the engine targets).
	 * @return Whether the render graph ran this frame's passes; false when the feature uses its D3D11 path this frame.
	 */
	bool AfterDeferredPasses();

	/** @brief The shader defines changed (the feature's ClearShaderCache): the programs are built again at the next frame. */
	void ClearShaderCache();

	~ORGSubsurfaceScattering();

private:
	ORGSubsurfaceScattering() = default;

	/** @brief Leaves the ORG path: the client unregistered, the state dropped. */
	void Stop();

	struct Impl;
	std::unique_ptr<Impl> impl;
};

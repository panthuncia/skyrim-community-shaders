#pragma once

#include "RenderGraph/Port.h"

#include <d3d11.h>
#include <functional>
#include <memory>

/**
* @brief Subsurface Scattering on the render graph at DeferredPasses, in graph mode.
*
* Separable mode extracts diffuse, then blurs horizontally into a temporary and vertically back into the main target.
* Burley mode clears, extracts masked pixels, and blurs them back. The engine targets are imported so D3D11 readers see the result.
* The commit latches the constants and opens the gate only on frames with skin.
* The working textures are declared on the display size, the programs and the client on SSMode, so a change switches in with a
* frame revision. In graph mode any failure is fatal. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGSubsurfaceScattering : public RenderGraph::Port<ORGSubsurfaceScattering>
{
public:
	static constexpr std::string_view kName = "Subsurface Scattering";

	/**
	* @brief At SetupResources, once a session. The client is registered at setup's build, so every frame has it.
	* @return Whether the graph path runs: false outside graph mode. In graph mode a failure is fatal.
	*/
	bool Setup(const D3D11_TEXTURE2D_DESC& a_desc);

	/**
	* @brief At DrawSSS, after the anchor ran. @return False outside graph mode: D3D11 draws. In graph mode always true.
	*/
	bool AfterDeferredPasses();

	/** @brief The client's commit, on the render thread: this frame's DeferredPasses ran the passes. */
	void NoteRan();
	~ORGSubsurfaceScattering();

private:
	friend class RenderGraph::Port<ORGSubsurfaceScattering>;
	ORGSubsurfaceScattering();

	struct Impl;
	std::unique_ptr<Impl> impl;
};

#pragma once

#include "Features/HDRDisplay.h"

#include <d3d11.h>
#include <functional>
#include <memory>

namespace org
{
	class ExternalTextureResource;
}

/**
 * @brief HDR Display's output on the render graph (CS_ORG_HDR_DISPLAY=1).
 *
 * What HDR Display issues on the GPU, and where it runs here:
 * - ApplyHDR's composite (HDROutputCS: the scene and the UI texture into the output texture), the copy of the output into the
 *   swap chain's back buffer, and the UI texture's clear after the composite (ClearUIBuffer's) are passes at the HDROutput anchor,
 *   which ApplyHDR runs (RenderGraph::AnchorPasses: a dispatch, a copy and a UAV clear, order keys from 100 up). Only Compose runs
 *   the anchor, so every pass runs at every run: no run gate.
 * - The owned frame (CS_ORG_OWNED_FRAME=1) presents the output itself (OwnedFrame::SetFrameSource): no copy pass then.
 * - These stay on D3D11: the UI texture's clear and redirect when the engine starts its interface draw (SetUIBuffer), and the
 *   scene's clear before a Bink movie draws, because they sit inside the engine's own D3D11 UI and movie draws (Phase 5); the
 *   clean composite for screenshots and frame generation's hudless colour (ComposeCleanCapture), which Screenshot's and
 *   Upscaling's ports take; the copy of the framebuffer when HDROutputCS is missing.
 *
 * Resources: the scene (HDR's float texture, or the framebuffer when HDR is off), the output texture, the UI texture and the back
 * buffer are D3D11 textures imported through FrameAnchors::D3D11Texture. The UI texture is cleared through its UAV.
 * The per-frame constants (HDRDisplay::HDRDataCB) are a latch region; SharedData comes from the anchor's shared constants.
 *
 * Setup follows the ports' template: Reserve at PostPostLoad, Setup (the program and the latch) at SetupResources, IsActive
 * checks the runtime. The client is registered at the first Compose, and again when a texture or the screen size changed.
 *
 * On any failure ApplyHDR composes with D3D11, that frame or for good. Render thread throughout.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGHDRDisplay
{
public:
	/** @brief Whether CS_ORG_HDR_DISPLAY=1 is set. */
	static bool Requested();

	/** @brief Reserves the HDROutput anchor when requested. Call before the render graph initializes (PostPostLoad). */
	static void Reserve();

	/** @brief The one instance, for the session. */
	static ORGHDRDisplay& Get();

	/**
	 * @brief At SetupResources, with the runtime active: builds HDROutputCS for the graph and makes the latch. Nothing when not
	 * requested or already set up.
	 * @param a_onFailed Called once if the path stops later (the graph failed, an import failed).
	 * @return Whether Compose may run the graph path; false keeps HDR Display on D3D11.
	 */
	bool Setup(std::function<void()> a_onFailed = {});

	/** @brief Whether the graph path is set up, has not failed, and the runtime is still active. */
	bool IsActive() const;

	/** @brief What Compose did on the graph. */
	enum class Composed
	{
		None,                 // nothing: ApplyHDR composes and copies with D3D11
		Output,               // the composite and the UI clear; the caller copies into the back buffer when it needs one
		OutputAndBackBuffer,  // the composite, the copy into the back buffer and the UI clear
	};

	/**
	 * @brief ApplyHDR on the graph: runs the HDROutput anchor with this frame's constants. Registers the client the first time,
	 * and again when a texture or the screen size changed. Composed::None while not IsActive.
	 * @param a_constants This frame's HDROutputCS constants (HDRDisplay::BuildHDRData).
	 * @param a_backBuffer The back buffer to copy the output into, or null (the owned frame presents the output itself).
	 */
	Composed Compose(const HDRDisplay::HDRDataCB& a_constants, ID3D11Texture2D* a_backBuffer);

	/** @brief Whether the last Compose cleared the UI texture, which ClearUIBuffer then leaves alone. Resets the answer. */
	bool TakeUIClear();

	/** @brief The program is built again from its file at the next Compose (HDRDisplay::ClearShaderCache). */
	void ClearShaderCache();

	/**
	 * @brief The graph resource the registered passes write a_texture through (the output texture, or the back buffer they copy into);
	 * null when they write no such texture. Another anchor's pass reading that texture uses this import: a second import of the same
	 * image would be a graph resource of its own, which nothing orders after these writes (ORGScreenshot's capture).
	 */
	std::shared_ptr<org::ExternalTextureResource> ImportOf(ID3D11Texture2D* a_texture) const;

	~ORGHDRDisplay();

	/** @brief The program, latch, back buffer import and registered inputs (ORGHDRDisplay.cpp). */
	struct Impl;

private:
	ORGHDRDisplay() = default;

	/** @brief Stops the ORG path for good: ApplyHDR composes with D3D11 from now on. */
	void Fail(const char* a_why);

	std::unique_ptr<Impl> impl;
	bool failed = false;
	bool graphFailed = false;  // the client's onFailed: the anchor's epoch failed inside Compose's Run
	bool uiCleared = false;
};

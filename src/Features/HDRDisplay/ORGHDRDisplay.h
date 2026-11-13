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
 *   the anchor, so these run at every run; only the scene clear below goes through the client's run gate.
 * - The owned frame (CS_ORG_OWNED_FRAME=1) presents the output itself (OwnedFrame::SetFrameSource): no copy pass then.
 * - A Bink movie frame's scene clear (PrepareMovieDraw) is a gated dispatch before the composite at the same anchor (ClearSceneCS,
 *   through the client's run gate): Bink draws into the UI texture, so the scene is untouched until the composite reads it.
 * - The UI texture's clear when the engine starts its interface draw (SetUIBuffer) is skipped while the texture is still clean from
 *   the graph's UI clear after the last composite: it clears on D3D11 only after a redirect this frame already drew into it.
 * - These stay on D3D11: the redirect itself (SetUIBuffer), inside the engine's own D3D11 UI draws; the clean composite for
 *   screenshots and frame generation's hudless colour (ComposeCleanCapture), which runs at Screenshot's and Upscaling's points with
 *   no HDR anchor there; the copy of the framebuffer when HDROutputCS is missing.
 *
 * Resources: HDR's float scene texture, the output texture and the UI texture are graph-owned twins (MakeTexture, "HDR::HdrTexture",
 * "HDR::OutputTexture", "HDR::UiTexture"), persistent and published, exported to D3D11, where HDR Display makes its views of them for
 * the engine's draws and its D3D11 paths. The composite, Bink's and Scaleform's UI passes and Upscaling's hudless copy declare the
 * twins themselves (GraphOf). A texture whose twin cannot be made is a D3D11 texture, imported through FrameAnchors::D3D11Texture,
 * as are the framebuffer (the scene when HDR is off) and the back buffer. The UI texture is cleared through its UAV.
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

	/** @brief HDR Display's own textures, which the graph owns when it can (MakeTexture). */
	enum class Texture
	{
		Scene,   // hdrTexture: ISHDR's float output
		Output,  // outputTexture: the composite, in the swap chain's format
		Ui,      // uiTexture: the interface, drawn apart
		Count
	};

	/**
	 * @brief At SetupResources, when requested and the runtime is active: makes a_texture as a graph-owned twin of a_desc, persistent,
	 * exported and published under a_name. Its D3D11 export lives until ReleaseTextures.
	 * @return A Buffer.h wrapper of the export (its texture only: HDR Display makes the views); null: make a D3D11 texture.
	 */
	Texture2D* MakeTexture(Texture a_texture, const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name);

	/**
	 * @brief HDR Display's DestroyResources, after its wrappers are gone: releases the twins once D3D11 is past them. The next
	 * SetupResources makes new ones, published under the same names (one graph rebuild).
	 */
	void ReleaseTextures();

	/**
	 * @brief The twin whose D3D11 export is a_texture (DXVK's), for a pass that reads or writes it; null for any other texture (a D3D11
	 * one: import it).
	 */
	std::shared_ptr<org::ExternalTextureResource> GraphOf(ID3D11Texture2D* a_texture) const;

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
	 * @param a_clearScene Clear HDR's scene to black before the composite (a movie frame's clear). With Composed::None the caller
	 * clears it with D3D11 before composing there.
	 */
	Composed Compose(const HDRDisplay::HDRDataCB& a_constants, ID3D11Texture2D* a_backBuffer, bool a_clearScene = false);

	/** @brief Whether the last Compose cleared the UI texture, which ClearUIBuffer then leaves alone. Resets the answer. */
	bool TakeUIClear();

	/**
	 * @brief Whether Compose can clear HDR's scene on the graph before the composite (its twin and the clear program exist): a movie
	 * frame's clear (HDRDisplay::PrepareMovieDraw) then waits for Compose instead of clearing with D3D11.
	 */
	bool CanClearScene() const;

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

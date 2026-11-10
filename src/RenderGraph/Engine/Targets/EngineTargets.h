#pragma once

#include <d3d11.h>

#include <cstdint>
#include <memory>

namespace org
{
	class ExternalTextureResource;
}

/**
 * @brief The engine's render targets as ORG resources (org-full-port.md P4.2; RE in docs/development/p4/render-targets.md).
 *
 * With CS_ORG_ENGINE_TARGETS=1, the engine's creation functions are detoured: Renderer::CreateRenderTarget (75467 / 77253),
 * CreateDepthStencilTarget (75469 / 77255), CreateCubeMapRenderTarget (75471 / 77257) and Create3DRenderTarget (75470 / 77256).
 * Each target is an image ORG creates (GraphResources::ExportTexture), described from the properties the engine passes: CS's
 * upgrades (the call-site hooks in Hooks.cpp, which rewrite those properties on their way in) are in the description that way.
 * The engine's RenderTargetData, DepthStencilData, CubemapRenderTargetData and Texture3DTargetData fields get DXVK's D3D11
 * objects of the same memory, so every D3D11 reader keeps working. With CS_ORG_ENGINE_TARGET_HANDLES=1 and the tripwire's guest
 * device and context installed, the fields hold tripwire handles that wrap those objects instead.
 *
 * Each target is published to every epoch's graph by id: "Engine::kMAIN", "Engine::kMAIN Copy", "Engine::Depth::kMAIN",
 * "Engine::Cubemap::kREFLECTIONS", "Engine::3D::VOLUMETRIC_LIGHTING_ACCUMULATION". FrameAnchors' engine imports and Drawcall
 * Limit Fix's imports return the published resource for an image ORG owns, so the graph orders their uses with each other.
 *
 * The first BSShaderRenderTargets::Create runs before the render graph exists, so the engine makes its targets with D3D11 then.
 * TakeOver replaces them once the graph is up, before any feature sets up its resources. Every later Create (a window resize,
 * which releases and recreates the whole set through Renderer::WindowSizeChanged) makes them on ORG directly, and EndSet
 * publishes the new set at once.
 *
 * Kept on D3D11 (the creation function runs unchanged): kFRAMEBUFFER (the swap chain's), targets with mip generation (DXVK
 * cannot wrap an image with D3D11_RESOURCE_MISC_GENERATE_MIPS), targets that view a mip of another target (their views are made
 * on the owner's texture, ORG's when it is), 3D targets that are not DEFAULT usage, and any target ORG fails to make (logged).
 *
 * Lifetime: the engine releases its targets (WindowSizeChanged, KillWindow, Shutdown, the volumetric lighting's 3D targets)
 * through the D3D11 objects. Each D3D11 texture carries a token in its private data. When DXVK destroys the texture, the token
 * queues the image, and ServiceRetired hands it to RenderGraphRuntime::ReleaseAfterD3D11, so the memory outlives D3D11's last
 * use of it whoever held the last reference.
 *
 * Everything but Enabled runs on the render thread.
 */
namespace RenderGraph::Engine::Targets
{
	/** @brief Whether CS_ORG_ENGINE_TARGETS=1 is set (the creation hooks are installed). */
	bool Enabled();

	/** @brief Installs the creation detours when Enabled. Call with the other engine hooks. */
	void Install();

	/** @brief Before BSShaderRenderTargets::Create: a new set of targets starts. */
	void BeginSet();

	/** @brief After BSShaderRenderTargets::Create: publishes the set made on ORG and moves the resize generation once. */
	void EndSet();

	/**
	 * @brief State::Setup, right after the render graph initialized: the targets the engine made on D3D11 before it existed are
	 * made again on ORG and swapped into the engine's fields, then the views of mip targets are made again on the new textures.
	 * Nothing when Enabled is false or the graph is not active.
	 */
	void TakeOver();

	/** @brief Whether ORG owns render target a_target (RE::RENDER_TARGET) now. */
	bool Owns(std::uint32_t a_target);

	/**
	 * @brief Makes render target a_target again on ORG with another description, for CS code that repurposes an engine target
	 * (Deferred's GBuffer targets, HDR Display's LDR upgrades). The engine's fields get the new texture and its views (an SRV, an RTV
	 * and a UAV, each in a_desc's format, as a_desc's bind flags ask). Until the engine's next Create.
	 * @param a_keepCopy Keep the target's copy texture (HDR Display replaces only the texture and its views); otherwise the copy
	 * goes, as Deferred's replacement releases it.
	 * @return False, with nothing changed, when ORG does not own a_target or a_desc is not a DEFAULT 2D texture of one slice and
	 * one mip: the caller then uses its D3D11 path.
	 */
	bool Recreate(std::uint32_t a_target, const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name, bool a_keepCopy = false);

	/** @brief Makes a_target again from the engine's own properties, undoing Recreate. False when ORG does not own it. */
	bool Restore(std::uint32_t a_target);

	/**
	 * @brief The published graph texture whose D3D11 texture is a_texture (an engine target's texture or copy, a depth, cube or 3D
	 * target's texture; a tripwire handle of one too), or null.
	 */
	std::shared_ptr<org::ExternalTextureResource> Find(const ID3D11Resource* a_texture);

	/** @brief Find for the VkImage DXVK reports for a texture (dxvkGetInteropResourceInfo's image), or null. */
	std::shared_ptr<org::ExternalTextureResource> FindImage(std::uint64_t a_image);

	/** @brief Render thread, each frame (the frame pace point): the images of destroyed D3D11 textures go to ReleaseAfterD3D11. */
	void ServiceRetired();
}

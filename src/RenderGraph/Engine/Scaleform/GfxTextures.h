#pragma once

/**
 * @brief Scaleform's own textures as ORG textures.
 *
 * While GTextureD3D1x's InitTexture or InitDynamicTexture runs, the guest device's CreateTexture2D makes the texture as an ORG
 * image, so the graph reads it by its record and nothing is imported per view. Render targets, dynamic and depth textures, and
 * textures made before the graph is up stay DXVK's. Without the tripwire guest nothing is patched on DXVK's device.
 */
namespace RenderGraph::Engine::Scaleform::GfxTextures
{
	/** @brief Hooks InitTexture and InitDynamicTexture (vtable SE 292188 / AE 243114, slots 1 and 2). */
	void Install();
}

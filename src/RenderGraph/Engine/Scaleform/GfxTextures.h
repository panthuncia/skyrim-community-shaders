#pragma once

/**
 * @brief Scaleform's own textures as ORG textures (docs/development/p5/scaleform.md, "Textures").
 *
 * GFx makes its textures through GTextureD3D1x (InitTexture from an image: gradients, images in the movie; InitDynamicTexture: the
 * glyph cache, an A8 texture it updates with UpdateSubresource) on the device its renderer holds, the tripwire guest device. While
 * one of those two runs, the guest device's CreateTexture2D (Tripwire::ScopedCreateTexture2DHook) makes a DEFAULT shader-resource
 * texture through Engine::Textures::Create: an ORG image with DXVK's texture on it, so the graph reads it by its record
 * (Engine::Textures::Find, through the view GFx makes on it) and nothing is imported from DXVK per view. Render targets, dynamic and
 * depth-stencil textures, and every texture made before the graph can wrap images stay DXVK's (GpuTextures imports their views
 * once each). Without the guest (CS_ORG_TRIPWIRE_GUEST=0) every GFx texture stays DXVK's: nothing is patched on DXVK's device.
 */
namespace RenderGraph::Engine::Scaleform::GfxTextures
{
	/**
	 * @brief At load, with CS_ORG_SCALEFORM=1, CS_ORG_ENGINE_TEXTURES=1 and CS_ORG_TRIPWIRE_GUEST=1: hooks GTextureD3D1x's
	 * InitTexture and InitDynamicTexture (vtable SE 292188 / AE 243114, slots 1 and 2), which set the guest device's texture hook
	 * while they run.
	 */
	void Install();
}

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace DCLF
{
	/** @brief Why an object stays on the native render loop. */
	enum class Ineligible : std::uint8_t
	{
		None,
		NotTriShape,         // BSDynamicTriShape, BSMultiIndexTriShape, LOD shapes, ...
		Skinned,             // has a skin instance
		NoRendererData,      // no GPU buffers yet
		NotLightingShader,   // effect, water, sky, grass, ...
		Technique,           // technique outside the supported set
		AlphaBlend,          // transparency
		Decal,               // decal batches
		ProjectedUV,         // snow/moss projection
		UnsupportedParent,   // under a NiSwitchNode or BSOrderedNode
		Hidden,              // app-culled or hidden this frame (per frame)
		Fading,              // fade node not fully faded in (per frame)
		Count
	};

	constexpr std::array<std::string_view, static_cast<std::size_t>(Ineligible::Count)> kIneligibleNames{
		"eligible",
		"not-trishape",
		"skinned",
		"no-renderer-data",
		"not-lighting-shader",
		"technique",
		"alpha-blend",
		"decal",
		"projected-uv",
		"unsupported-parent",
		"hidden",
		"fading",
	};

	struct LightingDescriptors
	{
		std::uint32_t technique = 0;  // descriptor bits 24-29, as GetRenderPasses selects it
		std::uint32_t pass = 0;       // pass descriptor (BSRenderPass::passEnum - 0x4800002D), before the VS/PS split
		std::uint32_t vertex = 0;     // final descriptors, as the native deferred draw looks the shaders up
		std::uint32_t pixel = 0;
		float specularLODFade = 1.0f;  // what GetRenderPasses stores in the property for the PS constants
		float envmapLODFade = 1.0f;
	};

	/** @brief Re-reads the [LightingShader] LOD fade settings (once per frame). */
	void RefreshLodFadeSettings();

	/** @brief Technique GetRenderPasses selects from the property flags (engine notes: technique table). */
	std::uint32_t SelectLightingTechnique(std::uint64_t a_flags);

	/**
	 * @brief Derives the vertex and pixel descriptors the native main (deferred) pass uses for this
	 * geometry, without running GetRenderPasses. Returns the reason when Phase 1 does not handle it.
	 */
	Ineligible DeriveLightingDescriptors(const RE::BSLightingShaderProperty& a_property, const RE::BSGeometry& a_geometry, LightingDescriptors& a_out);
}

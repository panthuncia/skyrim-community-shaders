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
		Actor,               // part of an actor's 3D (carried items; interiors keep actors in the rooms)
		AlphaTestState,      // in an alpha-test batch list without DoAlphaTest: the drawn technique is not known up front (per frame)
		Lod,                 // object or landscape LOD
		UnstableBuffer,      // a vertex or index buffer DXVK cannot make stable (the game can map it)
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
		"actor",
		"alpha-test-state",
		"lod",
		"unstable-buffer",
	};

	struct LightingDescriptors
	{
		std::uint32_t technique = 0;  // descriptor bits 24-29, as GetRenderPasses selects it
		std::uint32_t pass = 0;       // pass descriptor (BSRenderPass::passEnum - 0x4800002D), before the VS/PS split
		std::uint32_t vertex = 0;     // final descriptors, as the native deferred draw looks the shaders up
		std::uint32_t pixel = 0;
		std::uint32_t rawVertex = 0;  // descriptors BeginTechnique receives, before State::ModifyShaderLookup
		std::uint32_t rawPixel = 0;
		float specularLODFade = 1.0f;  // what GetRenderPasses stores in the property for the PS constants
		float envmapLODFade = 1.0f;
		std::uint32_t derivedPass = 0;  // the pass descriptor derived from the property (kNotDerived if it cannot be)
	};

	/** @brief LightingDescriptors::derivedPass when the derivation leaves the object native. */
	inline constexpr std::uint32_t kNotDerived = ~0u;

	/** @brief FadeStateOf: the object has a fade-sensitive flag but no fade node, or a non-finite metric. */
	inline constexpr std::uint8_t kFadeNoNode = 0x40;
	inline constexpr std::uint8_t kFadeInvalid = 0x80;

	/**
	 * @brief The whole camera dependence of the classification, in one byte.
	 *
	 * Two bits say whether the specular and environment-map LOD fades have run out, which is all the fade
	 * metric contributes to the derived technique. An object with none of the fade-sensitive flags has no
	 * camera dependence and returns 0 without reading the fade node. Comparing this per frame is what lets
	 * the rest of the classification be cached.
	 */
	std::uint8_t FadeStateOf(const RE::BSShaderProperty* a_property);

	/**
	 * @brief Pass descriptor bits GetRenderPasses sets from per-frame engine state rather than from the
	 * property: ShadowDir (13), DefShadow (14) and the shadow light count (6-8) from the light and shadow
	 * assignment, and DoAlphaTest (20), which for alpha-tested geometry also depends on the early-Z
	 * global other renders toggle (engine notes: GetRenderPasses runtime bits).
	 */
	inline constexpr std::uint32_t kRuntimePassBits = 0x1061c0u;

	/** @brief Pass descriptor of a BSRenderPass (passEnum minus the Lighting shader's base). */
	inline constexpr std::uint32_t PassDescriptorOf(std::uint32_t a_passEnum) { return a_passEnum - 0x4800002Du; }

	/** @brief Re-reads the [LightingShader] LOD fade settings (once per frame). */
	void RefreshLodFadeSettings();

	/** @brief Technique GetRenderPasses selects from the property flags (engine notes: technique table). */
	std::uint32_t SelectLightingTechnique(std::uint64_t a_flags);

	/**
	 * @brief Derives the vertex and pixel descriptors the native main (deferred) pass uses for this
	 * geometry, without running GetRenderPasses. Returns the reason when Phase 1 does not handle it.
	 */
	/**
	 * @brief Where the main-camera accumulator holds a geometry's lighting pass this frame.
	 * technique is the batch group's key, the pass descriptor SetupTechnique receives (it can differ from
	 * the pass's own passEnum: the accumulator adds DoAlphaTest when it registers the pass).
	 */
	struct AccumulatedPass
	{
		const RE::BSRenderPass* pass = nullptr;
		std::uint32_t technique = 0;  // pass descriptor (key minus the Lighting base)
		std::uint32_t subPass = 0;    // PassGroup list 0-4; the renderer draws 1, 3 and 4 with alpha testing
		std::uint32_t passEnum = 0;   // the pass's own passEnum when the tables were built (diagnostics)
	};

	/**
	 * @param a_accumulated The geometry's accumulated pass this frame (SceneStore::FindAccumulatedPass), or
	 * null. When present its technique is the pass descriptor (it is what the draw uses); the derivation
	 * from the property is kept in derivedPass for comparison. Without it the derivation stands, with its
	 * guesses for the kRuntimePassBits.
	 */
	Ineligible DeriveLightingDescriptors(const RE::BSLightingShaderProperty& a_property, const RE::BSGeometry& a_geometry,
		const AccumulatedPass* a_accumulated, LightingDescriptors& a_out);
}

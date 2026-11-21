#pragma once

// Ports of the engine's pass and shader-descriptor selection: why an object stays native, the Lighting descriptors DCLF draws it with,
// LOD fades and particle acceptance. Pure functions of the capture records, run on the scene-work thread and its pool.
// The render thread samples the frame inputs.

#include <array>
#include <cstdint>
#include <string_view>

#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

namespace DCLF::SceneCapture
{
	struct PropertyRecord;
	struct LeafView;
	struct AlphaRecord;
}

namespace DCLF
{
	/** @brief Why an object stays on the native render loop. */
	enum class Ineligible : std::uint8_t
	{
		None,
		NotTriShape,         // BSDynamicTriShape, LOD shapes, ...
		Skinned,             // has a skin instance
		NoRendererData,      // no GPU buffers yet
		NotLightingShader,   // effect, water, sky, grass, ...
		Technique,           // technique outside the supported set
		AlphaBlend,          // transparency
		Decal,               // decal batches
		UnsupportedParent,   // a blended leaf under a BSOrderedNode (its opaque children are eligible)
		Hidden,              // app-culled or hidden this frame (per frame)
		Fading,              // fade node not fully faded in (per frame)
		Actor,               // part of an actor's 3D
		Lod,                 // object or landscape LOD
		UnstableBuffer,      // a buffer DXVK cannot make stable (the game can map it)
		SkinShape,           // a skin the skinned path does not take: dismember instance, several partitions, too many bones
		Billboard,           // under an NiBillboardNode (the main cull turns it to the camera)
		Switch,              // under an NiSwitchNode not selecting it (or its leaves not admitted: Features/SwitchNodes)
		Held,                // a CS feature redraws it after the opaque pass (Api::HeldGroupContribution) and a decal
		MultiIndex,          // BSMultiIndexTriShape: its main pass is the engine's (the snow layer), its shadow a tri-shape's
		EffectOpaque,        // an effect without a blending alpha property
		EffectVariant,       // an effect permutation not ported
		WaterUnclaimed,      // water the port classifies but its claim is off (CS_DCLF_WATER=1, phase 1)
		WaterVariant,        // water the port does not draw: wading, a bad stencil bit, lights without Light Limit Fix
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
		"unsupported-parent",
		"hidden",
		"fading",
		"actor",
		"lod",
		"unstable-buffer",
		"skin-shape",
		"billboard",
		"switch",
		"terrain-no-blend",  // Held's: Integrations/TerrainBlending's groups, the only held ones (the census's text kept)
		"multi-index",
		"effect-opaque",
		"effect-variant",
		"water-unclaimed",
		"water-variant",
	};

	struct LightingDescriptors
	{
		std::uint32_t technique = 0;  // descriptor bits 24-29, as GetRenderPasses selects it
		std::uint32_t pass = 0;       // BSRenderPass::passEnum - 0x4800002D, before the VS/PS split
		std::uint32_t vertex = 0;     // final descriptors, as the native deferred draw looks shaders up
		std::uint32_t pixel = 0;
		std::uint32_t rawVertex = 0;  // what BeginTechnique receives, before State::ModifyShaderLookup
		std::uint32_t rawPixel = 0;
		std::uint32_t derivedPass = 0;  // kNotDerived if it cannot be derived
		// The technique behind Ineligible::Technique, or kRefractionReject.
		std::uint32_t rejectedTechnique = 62;  // 62 means never set, so an empty field cannot read as none
		// Decal group: 1 for hint 2 opaque, 2 for hint 3 blended, with the blend modes the group function and SetupGeometry leave.
		// The depth-bias mode is frame state.
		std::uint32_t decalGroup = 0;
		std::uint32_t decalBlendMode = 0;
		std::uint32_t decalWriteMode = 0;
		// An opaque object, hint 0, drawn through decal group 1 without depth bias.
		bool opaqueInDecalGroup = false;
		// The terrain group under Terrain Blending: 1 kMultiTextureLandscape, 2 kNoTransparencyMultiSample, sorted as its hook sorts them.
		std::uint32_t terrainGroup = 0;
		// A blended object the forward epoch draws, with the engine's alphaBlendMode and depth flags.
		bool forward = false;
		std::uint32_t forwardBlendMode = 0;
		bool forwardDepthTest = false, forwardDepthWrite = false;
		// A first-person effect decal drawn forward, with its decal group.
		std::uint32_t forwardDecalGroup = 0;
		// Drawn as GetRenderPasses' Utility RENDER_NORMAL pass, with the FALLOFF (bit 16) and CLAMP (bit 13) variants.
		// Its Lighting descriptors are never drawn.
		bool refraction = false;
		bool refractionFalloff = false, refractionClamp = false;
		// A material port's object kind, 0 for Lighting. Pass and descriptors come from its shader, and technique is 0.
		std::uint8_t port = 0;
		// A port's own key bits, put in its program family's spare bits (kProgramFamilySpareBits of them), such as water's stencil
		// bit index. Only for a family without turnedKeyBits, which use the same bits.
		std::uint8_t familyKeyBits = 0;
		// ProjectedUV needs the per-object texture matrix, pixel parameters and projected textures.
		bool projectedUV = false;
	};

	/** @brief rejectedTechnique for the refraction rejection, which has no technique. */
	inline constexpr std::uint32_t kRefractionReject = 63;

	/** @brief derivedPass when the derivation leaves the object native. */
	inline constexpr std::uint32_t kNotDerived = ~0u;

	/** @brief The object has a fade-sensitive flag but no fade node. */
	inline constexpr std::uint8_t kFadeNoNode = 0x40;

	/** @brief kFadeNoNode or 0. The LOD metric is not read, because the draw does its own fades, so this stays camera-independent. */
	std::uint8_t FadeStateOf(const SceneCapture::PropertyRecord* a_property);

	/**
	 * @brief Pass bits GetRenderPasses sets from per-frame state, not the property: ShadowDir (13), DefShadow (14), shadow light count (6-8)
	 * and DoAlphaTest (20), which for alpha-tested geometry also follows an early-Z global other renders toggle.
	 */
	inline constexpr std::uint32_t kRuntimePassBits = 0x1061c0u;
	/** @brief Bits a registered pass supplies over the derivation: the runtime bits, screen-door fade (23) and the snow bits 19 and 21. */
	inline constexpr std::uint32_t kRegisteredPassBits = (1u << 20) | (1u << 23) | (1u << 19) | (1u << 21);
	/**
	 * @brief The light-independent shadow bits GetRenderPasses (AE 0x1414adfb0) would set. DefShadow alone adds one shadow light,
	 * which makes the technique bind the shadow mask. BuildDrawsCS decides per frame. a_property is a layer's property, or null.
	 */
	std::uint32_t StaticShadowBits(const SceneCapture::LeafView& a_leaf, bool a_settled, const SceneCapture::PropertyRecord* a_property = nullptr);
	/** @brief The accumulation hint of a multi-index shape's layer passes (FUN_1414b2330, read by FUN_1414f2ad0). */
	inline constexpr std::uint32_t kLayerHint = 12;
	/** @brief A BSMultiIndexTriShape's additional property, as FUN_1414b2330 takes it. Null otherwise or without a second index list. */
	RE::BSShaderProperty* LayerPropertyOf(const RE::BSGeometry& a_geometry);
	inline constexpr std::uint32_t kShadowBits = 0x61c0u;  // ShadowDir, DefShadow and the shadow light count

	/**
	 * @brief Inputs for the draw's specular and envmap LOD fades, as BSFadeNode::OnVisible (FUN_14147b110) and GetRenderPasses (FUN_14147c470) give them.
	 * DCLF keeps both features at any distance and fades to 0 past the end, which matches a dropped feature. Uploaded to PS b13, c6-c12.
	 */
	struct LodFadeFrame
	{
		float eye[3]{};           // main camera world position
		float lodAdjust = 1.0f;   // NiCamera::lodAdjust
		float specularStart = 0.0f, specularEnd = 0.0f, envmapStart = 0.0f, envmapEnd = 0.0f;
		float metricScale = 0.0f;     // metric per scaled distance (0x141aa6300)
		float defaultScale = 0.0f;    // scale when the LOD type's divisor is not positive (0x141ad2840)
		float metricOverride = 0.0f;  // metric every node takes when overridden (0x14332a254 against 0x141769578)
		float overridden = 0.0f;      // 1 when metricOverride applies
		float divisors[16]{};         // per LOD type (+0x153 & 0xF), lodAdjust / divisor (0x142032e00)
		// 1 when BSFadeNode::OnVisible updates the metric (0x142032dfd). Otherwise nodes keep their fades.
		float fadesOn = 0.0f;
		// GetRenderPasses' refraction LOD fade thresholds.
		float refractionStart = 0.0f, refractionEnd = 0.0f;
		float pad = 0.0f;
	};
	static_assert(sizeof(LodFadeFrame) == 128);
	/** @brief Render thread. This frame's inputs, from the main camera and the engine's settings. */
	LodFadeFrame SampleLodFadeFrame();
	/** @brief The draw's LOD fade word: LOD type and which fades apply. */
	inline constexpr std::uint32_t kLodFadeTypeMask = 0xFu;
	inline constexpr std::uint32_t kLodFadeSpecular = 1u << 4;  // MaterialData.y, and SSRParams.w with kLodFadeSsr
	inline constexpr std::uint32_t kLodFadeEnvmap = 1u << 5;    // MaterialData.x
	inline constexpr std::uint32_t kLodFadeSsr = 1u << 6;       // the pass writes SSRParams.w, render flag 2 clear
	/**
	 * @brief The fade node's world bound centre, with the LOD type in w plus kLodFadeHeld when fades do not apply.
	 * w < 0 means no fade node. kIgnoreFade nodes are skipped by BSFadeNode::OnVisible.
	 */
	std::array<float, 4> LodFadeNodeOf(const RE::BSShaderProperty* a_property);
	inline constexpr float kLodFadeHeld = 16.0f;
	/** @brief Whether a LodFadeNodeOf row means the draw fades the property's LOD fades. */
	inline bool LodFadesApply(const std::array<float, 4>& a_node) { return a_node[3] >= 0.0f && a_node[3] < kLodFadeHeld; }

	/** @brief A BSRenderPass's pass descriptor: passEnum minus the Lighting shader's base. */
	inline constexpr std::uint32_t PassDescriptorOf(std::uint32_t a_passEnum) { return a_passEnum - 0x4800002Du; }

	/** @brief The technique GetRenderPasses selects from the property flags. */
	std::uint32_t SelectLightingTechnique(std::uint64_t a_flags);
	/**
	 * @brief The descriptor BSLightingShader::SetupTechnique (AE 0x1414db810) draws a pass with.
	 * LODLandNoise (18) becomes LODLand (9) unless the land noise global (0x142032fdb) is set.
	 * ParallaxOcc (7) becomes None (0) unless the parallax occlusion global (0x142035500) is set.
	 */
	std::uint32_t SetupTechniqueDescriptor(std::uint32_t a_pass);
	/** @brief SetupTechnique's VS/PS split, then State::ModifyShaderLookup. The pixel Deferred bit only when a_deferred. */
	void LightingShaderDescriptors(std::uint32_t a_pass, bool a_deferred, std::uint32_t& a_vertex, std::uint32_t& a_pixel);
	/** @brief The pixel descriptor's Deferred bit, the G-buffer variant. */
	inline constexpr std::uint32_t kLightingPixelDeferred = 1u << 4;
	/** @brief Whether the water reflection's cube map draws this technique. */
	inline constexpr bool LodLightingTechnique(std::uint32_t a_passDescriptor)
	{
		const std::uint32_t technique = (a_passDescriptor >> 24) & 0x3f;
		return technique == 9 || technique == 13 || technique == 15 || technique == 18;
	}
	constexpr std::array<std::string_view, 20> kLightingTechniqueNames{ "none", "envmap", "glowmap", "parallax", "facegen",
		"facegenRGBTint", "hair", "parallaxOcc", "MTLand", "LODLand", "snow", "multilayerParallax", "treeAnim", "LODObjects",
		"multiIndexSparkle", "LODObjectHD", "eye", "cloud", "LODLandNoise", "MTLandLODBlend" };
	inline std::string_view LightingTechniqueName(std::uint32_t a_technique)
	{
		return a_technique == 63 ? std::string_view("refraction") : a_technique < kLightingTechniqueNames.size() ? kLightingTechniqueNames[a_technique] : std::string_view("?");
	}

	/** @brief A member's main pass built from the object alone: what GetRenderPasses would register. */
	struct AccumulatedPass
	{
		std::uint32_t technique = 0;  // key minus the Lighting base, which SetupTechnique receives
		std::uint32_t subPass = 0;    // PassGroup list 0-4. The renderer alpha-tests 1, 3 and 4.
		std::uint32_t passEnum = 0;   // diagnostics only
		// BSRenderPass+0x1C: the batch renderer's geometry group, and which decal group for decals.
		std::uint32_t hint = 0;
		// LODMode as a row of the engine's partition table, index + singleLevel * 4. Picks the skin partitions the main camera draws.
		std::uint32_t lodRow = 3;
		// A synthetic pass whose technique carries the sun's bits, for the GPU to drop on a cascade miss.
		bool sunTest = false;
		// A resident pass. The accumulate phase patches its record once and keeps the patch.
		bool resident = false;
		// A resident pass under a fade root: the distance BuildDraws tests, or 0 when no test is needed.
		float fadeDistance = 0.0f;
		// A resident tree pass: BuildDraws applies BSTreeNode::OnVisible's height test.
		bool heightTest = false;
	};

	/** @brief A membership pass's derived descriptor with the inputs it came from. Derived again when any input differs. */
	struct MembershipDerived
	{
		const void* property = nullptr;
		const void* material = nullptr;
		std::uint64_t flags = 0;
		std::uint8_t fadeState = 0;
		std::uint32_t derivedPass = kNotDerived;

		bool operator==(const MembershipDerived&) const = default;
	};

	inline constexpr std::uint32_t kPassDoAlphaTest = 1u << 20;
	inline constexpr std::uint32_t kPassAdditionalAlphaMask = 1u << 23;  // screen-door fade
	inline constexpr std::uint32_t kPassProjectedUV = 1u << 15;

	/**
	 * @brief Hair (6) with ProjectedUV. SetupGeometry writes the projected-UV constants for every technique but Hair, so the native draw
	 * uses stale ones. DCLF draws such hair without projection.
	 */
	inline bool HairProjection(std::uint32_t a_descriptor)
	{
		return ((a_descriptor >> 24) & 0x3f) == 6 && (a_descriptor & kPassProjectedUV) != 0;
	}

	/**
	 * @brief The descriptor DCLF draws a pass in batch list a_subPass with. The renderer alpha-tests lists 1, 3 and 4 whatever the technique says.
	 * GetRenderPasses' DoAlphaTest flickers with the early-Z global, blending and fades while the pixels do not, so DCLF always adds the discard there.
	 */
	inline std::uint32_t DrawnPassDescriptor(std::uint32_t a_descriptor, std::uint32_t a_subPass)
	{
		if (HairProjection(a_descriptor))
			a_descriptor &= ~kPassProjectedUV;
		return (a_subPass == 1 || a_subPass == 3 || a_subPass == 4) ? (a_descriptor | kPassDoAlphaTest) : a_descriptor;
	}

	/** @brief GetRenderPasses' hint for a fading pass: geometry group 7, drawn blended just before the blended list. */
	inline constexpr std::uint32_t kFadingHint = 9;

	/**
	 * @brief Whether GetRenderPasses (AE 0x1414adfb0) gives a main pass hint 9: its fade node is fading, and it is not a decal, refraction,
	 * bit-19 property or a screen-door fade of an unblended property. The engine then draws it blended, without depth write or shadow bits.
	 */
	bool BlendFading(const SceneCapture::LeafView& a_leaf, const SceneCapture::PropertyRecord* a_property);

	/** @brief The engine's alphaBlendWriteMode for the blended group. FUN_1414b3390 sets 11 before FUN_1414b39d0's draws. */
	inline constexpr std::uint32_t kForwardWriteMode = 11;

	/**
	 * @brief The native main pass's descriptors, derived without running GetRenderPasses. Returns why the object stays native, or None.
	 * Camera-independent.
	 *
	 * @param a_accumulated This frame's accumulated pass, or null. Supplies kRegisteredPassBits.
	 * @param a_layer a_property is the geometry's layer, drawn with hint 12.
	 */
	Ineligible DeriveLightingDescriptors(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf,
		const AccumulatedPass* a_accumulated, LightingDescriptors& a_out, bool a_layer = false);

	/**
	 * @brief alphaBlendMode as BSShader's alpha setup (AE FUN_14150bc80) picks it from a blending NiAlphaProperty, or 0 for pairs it leaves alone.
	 * Its mode 1 for a non-blending property with alpha below 1 is not modelled, so such objects are not eligible.
	 */
	std::uint32_t AlphaBlendModeOf(const SceneCapture::AlphaRecord& a_alpha);

	/** @brief Whether a geometry type is a particle type, kParticles or kStripParticles. */
	inline constexpr bool ParticleGeometryType(std::uint8_t a_type) { return a_type == 1 || a_type == 2; }
	/**
	 * @brief The palette rows class of a member's block. Only a skinless leaf whose capture is a complete particle system qualifies:
	 * an NiParticleSystem with particle data, an NiPSysData with velocities for aspect flags (FUN_140e51320), and a capacity.
	 * A world-space system's transform is the identity with its parent's scale (FUN_140dd4db0).
	 */
	Api::FrameValueItemClass PaletteItemClassOf(const SceneCapture::LeafView& a_leaf);
}

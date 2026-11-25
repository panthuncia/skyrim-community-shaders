#pragma once

// Effect shader pass descriptors and the variants DCLF cannot draw. Pure functions of the capture records, safe on any thread.

#include <array>
#include <cstdint>
#include <string_view>

namespace DCLF
{
	struct LightingDescriptors;
	enum class Ineligible : std::uint8_t;
	namespace Api
	{
		class InstallContext;
	}
	namespace SceneCapture
	{
		struct PropertyRecord;
		struct LeafView;
	}
}

namespace DCLF::Features::Effects
{
	/**
	 * @brief Builds the pass and shader descriptors as GetRenderPasses (0x1414bd890) and FUN_1414bde90 would.
	 * Returns why the pass stays native, or None.
	 */
	Ineligible DeriveEffectDescriptors(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, LightingDescriptors& a_out);

	/** @brief Effect variants DCLF does not draw. The blend pair counts only once the alpha property blends. */
	enum EffectVariantBit : std::uint32_t
	{
		kEffectVariantShape = 1u << 0,          // any shape but a BSTriShape or a particle system (dynamic shapes)
		kEffectVariantSkinned = 1u << 1,        // EffectSkinned disagrees with the leaf's skin instance (a rigid skin only without its switch)
		kEffectVariantDecal = 1u << 2,          // kDecal / kDynamicDecal that registers no pass
		kEffectVariantSkyObject = 1u << 3,      // bit 45: hint 0x12, geometry group 13
		kEffectVariantWireframe = 1u << 4,      // bit 48
		kEffectVariantCloudLighting = 1u << 5,  // retired: cloud-lit effects are drawn (the bit is kept for report names)
		kEffectVariantLightingNoLlf = 1u << 6,  // kEffectLighting without Light Limit Fix (PerGeometry PLight*)
		kEffectVariantBlood = 1u << 7,          // kWeaponBlood
		kEffectVariantProjectedUv = 1u << 8,    // kProjectedUV
		kEffectVariantBlendPair = 1u << 9,      // a blending pair the engine's alpha setup (FUN_14150bc80) leaves as it finds it
		// Particle rejection causes, see ParticleRejectionOf.
		kEffectVariantParticle = 1u << 10,
		kEffectVariantParticleClass = 1u << 11,
		kEffectVariantParticleData = 1u << 12,
		kEffectVariantParticleAligned = 1u << 13,
		kEffectVariantParticleUnblended = 1u << 14,
		kEffectVariantParticleDecal = 1u << 15,
		kEffectVariantCount = 16
	};
	constexpr std::array<std::string_view, kEffectVariantCount> kEffectVariantNames{ "shape", "skinned", "decal", "sky-object", "wireframe",
		"cloud-lighting", "lighting-without-llf", "blood", "projected-uv", "blend-pair", "particle: not ported", "particle: class",
		"particle: no data", "particle: aligned", "particle: unblended", "particle: decal" };
	inline constexpr std::uint32_t kEffectVariantParticles = kEffectVariantParticle | kEffectVariantParticleClass | kEffectVariantParticleData |
	                                                         kEffectVariantParticleAligned | kEffectVariantParticleUnblended | kEffectVariantParticleDecal;
	/**
	 * @brief Variants the port can draw behind a switch. Read once at install, before any classification.
	 * rigidSkins covers a skin instance without the Skinned bit, which the engine draws per partition with the unskinned shader.
	 */
	struct PortedVariants
	{
		bool rigidSkins = false;
	};
	void SetPortedVariants(const PortedVariants& a_variants);
	std::uint32_t EffectVariantsOf(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf);
	/** @brief Needs both the property's kSkinned and a skinned vertex description, as FUN_1414bde90 does. */
	bool EffectSkinned(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf);
	/** @brief Returns 0 when the particle pass is taken. */
	std::uint32_t ParticleRejectionOf(const SceneCapture::LeafView& a_leaf, std::uint64_t a_flags);
	/** @brief Reads false when the Light Limit Fix integration is not installed. */
	void NeedLightLimitFix(Api::InstallContext& a_context);
}

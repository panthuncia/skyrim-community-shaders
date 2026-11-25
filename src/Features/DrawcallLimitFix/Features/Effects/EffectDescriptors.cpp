// Decides whether an Effect shader pass can be drawn by DCLF, and builds its shader descriptors to match the engine's.
// Pure functions of the records, the live toggles and whether Light Limit Fix is loaded.

#include "Features/DrawcallLimitFix/Features/Effects/EffectDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Effects
{
	namespace
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;

		constexpr std::uint64_t Bit(Flag a_flag) { return static_cast<std::uint64_t>(a_flag); }

		Api::ClassifyInputId lightLimitFix;
		PortedVariants ported;

		struct EffectDraw
		{
			bool decal = false;
			bool blending = false;
			bool opaque = false;
			bool particles = false;
			std::uint32_t blendMode = 0;
		};

		// Returns why the pass stays native, or None with a_draw filled in.
		Ineligible EffectDrawOf(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, EffectDraw& a_draw)
		{
			const std::uint64_t f = a_property.flags;
			const auto* alpha = a_leaf.alpha;
			const std::uint32_t variants = EffectVariantsOf(a_property, a_leaf);
			if (variants & ~kEffectVariantBlendPair)
				return Ineligible::EffectVariant;
			// The engine uses hint 1 for blended passes, hint 0 for opaque, and hints 2 and 3 for opaque and blended decals.
			// Decals land in DCLF's decal groups 1 and 2.
			a_draw.decal = (f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) != 0;
			a_draw.blending = a_leaf.AlphaBlending();
			// Opaque effects such as forge coals draw through decal group 1. Only passes that are hint 0 every frame qualify.
			// When a fade drops property alpha below one the engine blends it, but DCLF keeps it opaque (dclf-architecture.md, "Opaque effects").
			const bool opaqueEffects = Extensions::ObjectClassOn(Api::ObjectClass::OpaqueEffect);
			a_draw.opaque = !a_draw.blending && !a_draw.decal;
			// GetRenderPasses (0x1414bd890) makes an unblended Falloff pass hint 1, so it goes to the forward epoch drawn unblended.
			if (a_draw.opaque && (f & Bit(Flag::kFalloff)) && !ParticleGeometryType(a_leaf.Type()))
				a_draw.opaque = false;
			if (a_draw.opaque && (!opaqueEffects || !Extensions::ObjectClassOn(Api::ObjectClass::Decal) || (f & Bit(Flag::kFalloff)) || !(f & Bit(Flag::kZBufferTest)) ||
									 !(f & Bit(Flag::kZBufferWrite)) || ParticleGeometryType(a_leaf.Type())))
				return Ineligible::EffectOpaque;
			// Everything else needs the forward epoch, as blended Lighting passes do.
			if (!a_draw.opaque && !a_draw.decal && !Extensions::ObjectClassOn(Api::ObjectClass::Forward))
				return Ineligible::AlphaBlend;
			a_draw.particles = ParticleGeometryType(a_leaf.Type());
			if (variants & kEffectVariantBlendPair)
				return Ineligible::EffectVariant;  // the engine's alpha setup leaves this blend pair as it finds it
			a_draw.blendMode = a_draw.blending ? AlphaBlendModeOf(*alpha) : 0u;
			return Ineligible::None;
		}

		// Mirrors the pass descriptor that FUN_1414bde90 builds.
		std::uint32_t EffectPassOf(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, bool a_particles)
		{
			const std::uint64_t f = a_property.flags;
			const auto* alpha = a_leaf.alpha;
			// Read the geometry's own vertex description at +0x148. A skinned shape's renderer data has a different one or none.
			const std::uint64_t vertexDesc = a_leaf.geometry ? a_leaf.geometry->shapeVertexDesc : 0;
			using namespace EffectFlag;
			std::uint32_t d = 0;
			if (f & Bit(Flag::kVertexColors))
				d |= Vc;
			if (a_property.feature & kEffectHasSourcePath)
				d |= TexCoord | Texture;
			if (EffectSkinned(a_property, a_leaf))
				d |= Skinned;
			if ((f & Bit(Flag::kFalloff)) && !a_particles)
				d |= Normals | Falloff;
			if (a_particles) {
				d |= Particles;
				if ((d & Texture) && a_leaf.geometry && a_leaf.geometry->particles.subtextures)
					d |= TexCoordIndex | IndexedTexture;
			}
			// Alpha property bits 1-4 are the source blend function and bits 5-8 the destination. They apply even when not blending.
			const std::uint32_t source = alpha ? (alpha->flags >> 1) & 0xF : ~0u, dest = alpha ? (alpha->flags >> 5) & 0xF : ~0u;
			if (dest == 0)
				d |= AddBlend;
			if (source == 4 && dest == 7)
				d |= MultBlendDecal;
			else if (dest == 2)
				d |= MultBlend;
			if (f & Bit(Flag::kSoftEffect))
				d |= Soft;
			if (f & Bit(Flag::kWeaponBlood))
				d |= Blood;
			// Also gated by global 0x142033508, assumed to stay at its initial value of 1.
			if (f & Bit(Flag::kEffectLighting))
				d |= Lighting;
			if (a_property.feature & kEffectHasGreyscalePath) {
				if (f & Bit(Flag::kGrayscaleToPaletteColor))
					d |= GrayscaleToColor;
				if (f & Bit(Flag::kGrayscaleToPaletteAlpha))
					d |= GrayscaleToAlpha;
			}
			constexpr std::uint64_t kVertexNormals = 0x200800000000000ull;
			if ((vertexDesc & kVertexNormals) && (!alpha || (alpha->flags & (1u << 9))) && (f & Bit(Flag::kZBufferTest)) && (f & Bit(Flag::kZBufferWrite)))
				d |= MotionVectorsNormals;
			return d;
		}

		void WriteEffectDescriptors(std::uint32_t a_pass, std::uint64_t a_flags, const EffectDraw& a_draw, LightingDescriptors& a_out)
		{
			using namespace EffectFlag;
			const std::uint32_t d = a_pass;
			// Split as SetupTechnique does (FUN_141521300, FUN_141521310), then cleared as State::ModifyShaderLookup does.
			// Deferred is set on the pixel stage only for decals and opaque effects.
			constexpr std::uint32_t kPermutationBits = GrayscaleToColor | GrayscaleToAlpha | IgnoreTexAlpha;
			a_out = {};
			a_out.port = kMaterialKindEffect;
			a_out.technique = 0;
			a_out.pass = d;
			a_out.derivedPass = d;
			a_out.rawVertex = d & 0xffc7ffffu;
			a_out.rawPixel = ((d & Membrane) ? d : (d & ~Skinned)) & ~SkyObject;
			a_out.vertex = a_out.rawVertex & ~kPermutationBits;
			a_out.pixel = (a_out.rawPixel & ~kPermutationBits) | ((a_draw.decal || a_draw.opaque) ? Deferred : 0u);
			if (a_draw.opaque) {
				a_out.opaqueInDecalGroup = true;
				a_out.decalGroup = 1u;
				a_out.decalBlendMode = 0u;
				a_out.decalWriteMode = 1u;
				return;
			}
			if (a_draw.decal) {
				// Matches BSEffectShader::SetupGeometry (0x1414eab10). Write mode is 11 without MotionVectorsNormals, else 10 for opaque.
				// See the engine notes, "Decals".
				a_out.decalGroup = a_draw.blending ? 2u : 1u;
				a_out.decalBlendMode = a_draw.blendMode;
				a_out.decalWriteMode = (!a_draw.blending && (d & MotionVectorsNormals)) ? 10u : 11u;
				return;
			}
			a_out.forward = true;
			a_out.forwardBlendMode = a_draw.blendMode;
			a_out.forwardDepthTest = (a_flags & Bit(Flag::kZBufferTest)) != 0;
			a_out.forwardDepthWrite = (a_flags & Bit(Flag::kZBufferWrite)) != 0;
		}
	}

	bool EffectSkinned(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf)
	{
		// Use the geometry's own vertex description (+0x148), as FUN_1414bde90 does. The renderer data's reads as unskinned for skinned shapes.
		constexpr std::uint64_t kVertexSkinned = 0x1004000000000000ull;
		const std::uint64_t vertexDesc = a_leaf.geometry ? a_leaf.geometry->shapeVertexDesc : 0;
		return (a_property.flags & Bit(Flag::kSkinned)) && (vertexDesc & kVertexSkinned);
	}

	std::uint32_t ParticleRejectionOf(const SceneCapture::LeafView& a_leaf, std::uint64_t a_flags)
	{
		// Particle passes are always hint 1, so an unblended one would take its blend mode from the frame's alpha (FUN_14150bc80).
		if (!Extensions::GeometryTypeOn(a_leaf.Type()))
			return kEffectVariantParticle;
		const auto* g = a_leaf.geometry;
		if (!g || a_leaf.Type() != static_cast<std::uint8_t>(RE::BSGeometry::Type::kParticles) || !g->particles.system || a_leaf.Skinned())
			return kEffectVariantParticleClass;
		std::uint32_t causes = 0;
		if (!g->particles.data || !g->particles.capacity)
			causes |= kEffectVariantParticleData;
		if (g->particles.aligned && !g->particles.velocities)
			causes |= kEffectVariantParticleAligned;
		if (!a_leaf.AlphaBlending())
			causes |= kEffectVariantParticleUnblended;
		if (a_flags & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal)))
			causes |= kEffectVariantParticleDecal;
		return causes;
	}

	std::uint32_t EffectVariantsOf(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf)
	{
		// Variants are what BSEffectShaderProperty::GetRenderPasses (0x1414bd890) and BSEffectShader's setup do that the port does not.
		const std::uint64_t f = a_property.flags;
		std::uint32_t variants = 0;
		if (ParticleGeometryType(a_leaf.Type())) {
			variants |= ParticleRejectionOf(a_leaf, f);
		} else if (a_leaf.Type() != static_cast<std::uint8_t>(RE::BSGeometry::Type::kTriShape)) {
			variants |= kEffectVariantShape;
		}
		// A skin instance without the Skinned bit is a rigid skin. The engine draws it with the unskinned shader, as rigidSkins does.
		// The Skinned bit without a skin instance would read bones nothing set, so it stays a variant.
		const bool effectSkinned = EffectSkinned(a_property, a_leaf);
		if ((a_leaf.Skinned() && !Extensions::ObjectClassOn(Api::ObjectClass::Skinned)) || (effectSkinned && !a_leaf.Skinned()) ||
			(!effectSkinned && a_leaf.Skinned() && !ported.rigidSkins))
			variants |= kEffectVariantSkinned;
		// kTempRefraction with kDynamicDecal registers no pass in the engine, so it stays a variant.
		if ((f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) &&
			(!Extensions::ObjectClassOn(Api::ObjectClass::Decal) || ((f & Bit(Flag::kTempRefraction)) && (f & Bit(Flag::kDynamicDecal)))))
			variants |= kEffectVariantDecal;
		if (f & Bit(Flag::kBillboard))
			variants |= kEffectVariantSkyObject;
		if (f & Bit(Flag::kWireframe))
			variants |= kEffectVariantWireframe;
		// kCloudLOD swaps the sun for the cloud light (ShadowSceneNode +0x208). Point lights come from Light Limit Fix's clusters only.
		if (f & Bit(Flag::kEffectLighting)) {
			if (!Extensions::ClassifyInputOn(lightLimitFix))
				variants |= kEffectVariantLightingNoLlf;
		}
		if (f & Bit(Flag::kProjectedUV))
			variants |= kEffectVariantProjectedUv;
		if (a_leaf.AlphaBlending() && AlphaBlendModeOf(*a_leaf.alpha) == 0)
			variants |= kEffectVariantBlendPair;
		return variants;
	}

	Ineligible DeriveEffectDescriptors(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, LightingDescriptors& a_out)
	{
		// See dclf-architecture.md, "The Effect shader in the forward epoch".
		EffectDraw draw;
		if (const Ineligible reason = EffectDrawOf(a_property, a_leaf, draw); reason != Ineligible::None)
			return reason;
		WriteEffectDescriptors(EffectPassOf(a_property, a_leaf, draw.particles), a_property.flags, draw, a_out);
		return Ineligible::None;
	}

	void SetPortedVariants(const PortedVariants& a_variants)
	{
		ported = a_variants;
	}

	void NeedLightLimitFix(Api::InstallContext& a_context)
	{
		lightLimitFix = a_context.NeedClassifyInput("light limit fix");
	}
}

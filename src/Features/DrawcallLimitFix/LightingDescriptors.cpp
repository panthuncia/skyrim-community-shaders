#include "LightingDescriptors.h"

#include "ShaderCache.h"
#include "State.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"

namespace DCLF
{
	namespace
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		using Technique = SIE::ShaderCache::LightingShaderTechniques;
		using LightingFlag = SIE::ShaderCache::LightingShaderFlags;

		constexpr std::uint64_t Bit(Flag a_flag) { return static_cast<std::uint64_t>(a_flag); }
		constexpr std::uint32_t Bit(LightingFlag a_flag) { return static_cast<std::uint32_t>(a_flag); }

		// Techniques Phase 1 handles. The rest (terrain, trees, faces, LOD, eyes, ...) stay native.
		constexpr bool IsSupportedTechnique(std::uint32_t a_technique)
		{
			switch (static_cast<Technique>(a_technique)) {
			case Technique::None:
			case Technique::Envmap:
			case Technique::Glowmap:
			case Technique::Parallax:
				return true;
			default:
				return false;
			}
		}

		// [LightingShader] thresholds GetRenderPasses compares the fade node's LOD metric against
		// (engine notes: LOD fades). Defaults are the engine's.
		struct LodFadeSettings
		{
			float specularStart = 0.09f;
			float specularEnd = 0.10f;
			float envmapStart = 0.09f;
			float envmapEnd = 0.10f;
		};
		LodFadeSettings lodFade;

		float ReadSetting(const char* a_name, float a_default)
		{
			auto* setting = RE::GetINISetting(a_name);
			return setting ? setting->GetFloat() : a_default;
		}

		// FUN_14147c470: false when the feature has faded out; otherwise the fade factor in a_fade.
		// a_metric is the fade node's current LOD metric (+0x144); the previous one (+0x148) only
		// feeds the engine's render-pass cache invalidation, which DCLF does not need.
		bool LodFadeVisible(float a_metric, float a_start, float a_end, float& a_fade)
		{
			if (a_end < a_metric)
				return false;
			if (a_metric <= a_start) {
				a_fade = 1.0f;
			} else {
				a_fade = std::clamp((a_metric - a_end) / (a_start - a_end), 0.0f, 1.0f);
			}
			return true;
		}

		float FadeNodeLodMetric(const RE::BSFadeNode* a_fadeNode)
		{
			// BSFadeNode+0x144 (RUNTIME_DATA unk144) holds a float despite CommonLib's integer type.
			return std::bit_cast<float>(a_fadeNode->GetRuntimeData().unk144);
		}

		// BSLightingShader::SetupTechnique splits the pass descriptor (engine notes, SetupTechnique).
		constexpr std::uint32_t VertexDescriptorFromPass(std::uint32_t d)
		{
			return (d & 0x48007u) | ((d & 0x20a00u) ? 0x200u : 0u) | (d & 0x3f000000u);
		}

		// Community Shaders replaces the pixel split (TruePBR.cpp, BSLightingShader_GetPixelTechnique) so that
		// bits 3-5 (its TruePbr/Deferred flags) survive; only the shadow light count (bits 6-8) is dropped.
		constexpr std::uint32_t PixelDescriptorFromPass(std::uint32_t d)
		{
			std::uint32_t p = d & ~0x1c0u;
			if ((p & 0x4u) == 0)  // Skinned is kept only with ModelSpaceNormals
				p &= ~0x2u;
			return p | 1u;  // VC is always set
		}
	}

	void RefreshLodFadeSettings()
	{
		lodFade.specularStart = ReadSetting("fSpecularLODFadeStart:LightingShader", 0.09f);
		lodFade.specularEnd = ReadSetting("fSpecularLODFadeEnd:LightingShader", 0.10f);
		lodFade.envmapStart = ReadSetting("fEnvmapLODFadeStart:LightingShader", 0.09f);
		lodFade.envmapEnd = ReadSetting("fEnvmapLODFadeEnd:LightingShader", 0.10f);
	}

	std::uint32_t SelectLightingTechnique(std::uint64_t f)
	{
		// Same order as BSLightingShaderProperty::GetRenderPasses; later tests win.
		std::uint32_t t = (f & Bit(Flag::kEnvMap)) ? 1u : 0u;
		if (f & Bit(Flag::kGlowMap))
			t = 2;
		if ((f & (Bit(Flag::kParallax) | Bit(Flag::kParallaxOcclusion))) == Bit(Flag::kParallax))
			t = 3;
		if (f & Bit(Flag::kFace))
			t = 4;
		if (f & Bit(Flag::kFaceGenRGBTint))
			t = 5;
		if (f & Bit(Flag::kHairTint))
			t = 6;
		if (f & Bit(Flag::kParallaxOcclusion))
			t = 7;
		if (f & Bit(Flag::kMultiTextureLandscape))
			t = 8;
		if (f & Bit(Flag::kNoLODLandBlend))
			t = 19;
		if (f & Bit(Flag::kLODLandscape))
			t = 18;
		if (f & Bit(Flag::kLODObjects))
			t = 13;
		if (f & Bit(Flag::kHDLODObjects))
			t = 15;
		if (f & Bit(Flag::kMultiLayerParallax))
			t = 11;
		if (f & Bit(Flag::kTreeAnim))
			t = 12;
		if ((f & (Bit(Flag::kMultiIndexSnow) | Bit(Flag::kProjectedUV))) == (Bit(Flag::kMultiIndexSnow) | Bit(Flag::kProjectedUV)))
			t = 14;
		if (f & Bit(Flag::kEyeReflect))
			t = 16;
		return t;
	}

	Ineligible DeriveLightingDescriptors(const RE::BSLightingShaderProperty& a_property, const RE::BSGeometry& a_geometry, LightingDescriptors& a_out)
	{
		std::uint64_t f = a_property.flags.underlying();

		if (f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal)))
			return Ineligible::Decal;
		if (f & Bit(Flag::kSkinned))
			return Ineligible::Skinned;
		if (f & Bit(Flag::kProjectedUV))
			return Ineligible::ProjectedUV;
		// Refraction has its own LOD fade that removes the object entirely; leave it to the game.
		if (f & (Bit(Flag::kRefraction) | Bit(Flag::kTempRefraction)))
			return Ineligible::Technique;

		// Distance LOD fades GetRenderPasses applies before choosing the technique: specular and the
		// environment map switch off past their [LightingShader] thresholds.
		float specularFade = a_property.specularLODFade;
		float envmapFade = a_property.envmapLODFade;
		// Without a fade node GetRenderPasses never computes these fades, so the property fields the draw
		// reads may be stale. Leave such objects native rather than guess.
		if (!a_property.fadeNode && (f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap))))
			return Ineligible::Fading;
		if (const auto* fadeNode = a_property.fadeNode) {
			const float metric = FadeNodeLodMetric(fadeNode);
			// A non-finite metric leaves the engine's fades undefined (its clamp keeps the NaN); leave it native.
			if (!std::isfinite(metric))
				return Ineligible::Fading;
			if ((f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow))) && !LodFadeVisible(metric, lodFade.specularStart, lodFade.specularEnd, specularFade))
				f &= ~Bit(Flag::kSpecular);
			if ((f & Bit(Flag::kEnvMap)) && !LodFadeVisible(metric, lodFade.envmapStart, lodFade.envmapEnd, envmapFade)) {
				// kSnow keeps the envmap past its fade distance, but then the engine never writes the fade and
				// the draw reads a stale property field. Leave that case native.
				if (f & Bit(Flag::kSnow))
					return Ineligible::Fading;
				f &= ~Bit(Flag::kEnvMap);
			}
		}

		const std::uint32_t technique = SelectLightingTechnique(f);
		if (!IsSupportedTechnique(technique))
			return Ineligible::Technique;

		const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		if (alpha && alpha->GetAlphaBlending())
			return Ineligible::AlphaBlend;

		// Pass descriptor as GetRenderPasses builds it for an opaque, fully faded-in object.
		// Light counts (bits 3-8) and the shadow bits never reach the deferred shaders, so they are left out.
		std::uint32_t d = technique << 24;
		if (f & Bit(Flag::kVertexColors))
			d |= Bit(LightingFlag::VC);
		if (f & Bit(Flag::kModelSpaceNormals))
			d |= Bit(LightingFlag::ModelSpaceNormals);
		if (f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow)))
			d |= Bit(LightingFlag::Specular);
		if (f & Bit(Flag::kSoftLighting))
			d |= Bit(LightingFlag::SoftLighting);
		if (f & Bit(Flag::kRimLighting))
			d |= Bit(LightingFlag::RimLighting);
		if (f & Bit(Flag::kBackLighting))
			d |= Bit(LightingFlag::BackLighting);
		if (f & Bit(Flag::kAnisotropicLighting))
			d |= Bit(LightingFlag::AnisoLighting);
		if (alpha && alpha->GetAlphaTesting())
			d |= Bit(LightingFlag::DoAlphaTest);

		// Community Shaders' GetRenderPasses hook (TruePBR.cpp): PBR materials swap Specular for TruePbr,
		// and glint turns on AnisoLighting.
		const auto* material = a_property.material;
		const bool isPbr = (f & Bit(Flag::kVertexLighting)) && material &&
		                   (material->GetFeature() == RE::BSShaderMaterial::Feature::kDefault ||
							   material->GetFeature() == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend);
		if (isPbr) {
			d |= Bit(LightingFlag::TruePbr);
			d &= ~Bit(LightingFlag::Specular);
			if (static_cast<const BSLightingShaderMaterialPBR*>(material)->glintParameters.enabled)
				d |= Bit(LightingFlag::AnisoLighting);
		}

		uint vertex = VertexDescriptorFromPass(d);
		uint pixel = PixelDescriptorFromPass(d);
		globals::state->ModifyShaderLookup(RE::BSShader::Type::Lighting, vertex, pixel, true);

		a_out.technique = technique;
		a_out.pass = d;
		a_out.vertex = vertex;
		a_out.pixel = pixel;
		a_out.specularLODFade = specularFade;
		a_out.envmapLODFade = envmapFade;
		return Ineligible::None;
	}
}

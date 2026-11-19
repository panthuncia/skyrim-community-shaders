// TruePBR: its material contributor, glints property bits, glints noise frame texture and Lighting pass rules. Capture and derive callbacks are pure.

#include "Features/DrawcallLimitFix/Integrations/TruePBR/TruePBRIntegration.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"

#include <cstdint>
#include <string_view>

#include "ShaderCache.h"
#include "TruePBR.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "TruePBR/ORGGlintsNoise.h"

namespace DCLF::Integrations::TruePBR
{
	namespace
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		using LightingFlag = SIE::ShaderCache::LightingShaderFlags;
		using Feature = RE::BSShaderMaterial::Feature;

		constexpr std::uint64_t Bit(Flag a_flag) { return static_cast<std::uint64_t>(a_flag); }
		constexpr std::uint32_t Bit(LightingFlag a_flag) { return static_cast<std::uint32_t>(a_flag); }

		// CS's pixel split keeps bits 3-5, TruePbr and Deferred. Only the shadow light count in bits 6-8 is dropped.
		constexpr std::uint32_t kPixelKeep = 0x38u;

		// The property capture with a material: the PBR test as CS's GetRenderPasses does it, then the PBR material's glint.
		std::uint8_t CaptureGlints(const RE::BSShaderProperty& a_property)
		{
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(a_property.material);
			if (!material)
				return 0;
			const auto feature = const_cast<RE::BSLightingShaderMaterialBase*>(material)->GetFeature();
			const bool pbr = a_property.flags.any(Flag::kVertexLighting) && (feature == Feature::kDefault || feature == Feature::kMultiTexLandLODBlend);
			return pbr && static_cast<const BSLightingShaderMaterialPBR*>(a_property.material)->glintParameters.enabled ? kGlints : std::uint8_t(0);
		}

		// CS's GetRenderPasses hook: PBR swaps Specular for TruePbr, and glint adds AnisoLighting.
		std::uint32_t DerivePass(std::uint32_t a_pass, const Api::LightingJoinView& a_view)
		{
			const auto feature = static_cast<Feature>(a_view.MaterialFeature());
			const bool isPbr = (a_view.PropertyFlags() & Bit(Flag::kVertexLighting)) && a_view.HasMaterial() &&
			                   (feature == Feature::kDefault || feature == Feature::kMultiTexLandLODBlend);
			std::uint32_t d = a_pass;
			if (isPbr) {
				d |= Bit(LightingFlag::TruePbr);
				d &= ~Bit(LightingFlag::Specular);
				if (a_view.contributorBits & kGlints)
					d |= Bit(LightingFlag::AnisoLighting);
			}
			return d;
		}

		// The glints noise TruePBR's Prepass binds at t20, made once. Before it exists, the captured view.
		// On the ORG path it is the published texture's export, so commits read it ordered after its fill.
		void* GlintsNoiseView()
		{
			const auto& truePBR = globals::features::truePBR;
			return truePBR.loaded ? truePBR.GlintsNoiseSRV() : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			// Its SetupMaterial replacement around vanilla: its classes, its landscape layers at t80-t91, and the TruePbr pass bit takesPass branches on.
			a_context.RegisterMaterialContributor({ .name = "TruePBR",
				.stage = Api::MaterialStage::Replace,
				.order = 0,
				.textureRegisters = kTextureRegisters,
				.signatureBits = Bit(LightingFlag::TruePbr),
				.materialClass = { .owns = &OwnClass, .bytes = &PBRMaterialBytes, .textureFields = &PBRTextureFields, .vanillaAsBase = true },
				.sample = &SampleFrame,
				.takesPass = &PBRTakesPass,
				.evaluate = &EvaluatePBR });
			a_context.RegisterPropertyBits({ .name = "glints", .bits = kGlints, .capture = &CaptureGlints });
			a_context.RegisterFrameTexture({ .name = "true pbr glints noise", .textureRegister = kGlintsNoiseRegister, .view = &GlintsNoiseView,
				.capturedWhenNull = true, .graphResources = { ORGGlintsNoise::kNoiseId } });
			// Its hooks' pass changes apply whether or not it is loaded, as their ports always did:
			// the GetRenderPasses swap, the GetPixelTechnique split, and SetupGeometry's raw technique.
			a_context.RegisterLightingPassRules({ .pixelKeep = kPixelKeep,
				.rawWhen = Bit(LightingFlag::TruePbr),
				.rawSet = Bit(LightingFlag::AmbientSpecular),
				.rawFlip = Bit(LightingFlag::AnisoLighting),
				.derivePass = &DerivePass });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "true pbr", .install = &Install, .status = [] { return FeatureStatus(globals::features::truePBR); } };
	}
}

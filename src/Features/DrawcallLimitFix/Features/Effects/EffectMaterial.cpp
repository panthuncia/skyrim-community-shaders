// The Effect port's material evaluation, a pure copy of BSEffectShader::SetupMaterial without the membrane half.
// Also registers the Effect material port.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPortFrame.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectDescriptors.h"
#include "Features/DrawcallLimitFix/Integrations/LinearLighting/LinearLighting.h"
#include "RenderGraph/Fatal.h"

#include <bit>
#include <cstring>

namespace DCLF::Features::Effects
{
	namespace
	{
		constexpr std::uint32_t kEffectMaterialBytes = 0x88;
		// BSEffectShaderMaterial fields that SetupMaterial (0x1414ea6c0) reads.
		constexpr std::uint16_t kFalloff = 0x38;  // start angle, stop angle, start opacity, stop opacity
		constexpr std::uint16_t kBaseColor = 0x48;
		constexpr std::uint16_t kSourceTexture = 0x58;
		constexpr std::uint16_t kGreyscaleTexture = 0x60;
		constexpr std::uint16_t kSoftFalloffDepth = 0x68;
		constexpr std::uint16_t kBaseColorScale = 0x6c;
		constexpr std::uint16_t kClampMode = 0x80;
		constexpr std::uint16_t kLightingInfluence = 0x81;
		// Texture transforms are double-buffered, 8 bytes apart, as in Lighting.
		constexpr std::uint16_t kTexcoordOffset = 0x0c;
		constexpr std::uint16_t kTexcoordScale = 0x1c;
		// 1/255 with the engine's exact bits (0x141ad2810).
		constexpr std::uint32_t kInverse255Bits = 0x3b808081u;
		constexpr std::uint32_t kFilterBilinear = 1;
		constexpr std::uint32_t kFilterAnisotropic = 3;
		constexpr std::uint16_t kEffectTextureFields[] = { kSourceTexture, kGreyscaleTexture };

		// Effect controllers change falloff and base colour every frame, so the frame owns these variables rather than the record.
		// No other record value reads those bytes. Linear Lighting converts the base colour as the record does.
		constexpr std::uint16_t kNoScale = Api::MaterialFrameComponent::kNoScale;
		constexpr Api::MaterialFrameVariable kEffectFrameVariables[] = {
			{ .pixel = false, .variable = kEffectVSFalloffData, .first = kFalloff, .end = kFalloff + 16,
				.components = { { { kFalloff, kNoScale }, { kFalloff + 4, kNoScale }, { kFalloff + 8, kNoScale }, { kFalloff + 12, kNoScale } } } },
			{ .pixel = true, .variable = kEffectPSBaseColor, .first = kBaseColor, .end = kBaseColor + 16,
				.components = { { { kBaseColor, kBaseColorScale }, { kBaseColor + 4, kBaseColorScale }, { kBaseColor + 8, kBaseColorScale }, { kBaseColor + 12, kNoScale } } },
				.alternateBits = EffectFlag::GrayscaleToColor,
				.alternate = { { { kBaseColor, kNoScale }, { kBaseColor + 4, kNoScale }, { kBaseColor + 8, kNoScale }, { kBaseColor + 12, kNoScale } } },
				.workingColour = true,
				.workingColourExceptBits = EffectFlag::GrayscaleToColor | EffectFlag::Blood },
		};

		const StageLayout& EffectVSLayout()
		{
			static const StageLayout layout = MakeStageLayout(kEffectVSVariables, kEffectVSSized, kEffectVSScratch);
			return layout;
		}

		const StageLayout& EffectPSLayout()
		{
			static const StageLayout layout = MakeStageLayout(kEffectPSVariables, {}, {});
			return layout;
		}

		// Returns false for membrane passes and for greyscale passes without a palette.
		bool EvaluateEffect(Api::MaterialPortEvaluation& a_port)
		{
			const std::uint32_t pass = a_port.passDescriptor;
			if (a_port.Port() != kMaterialKindEffect || a_port.Size() < kEffectMaterialBytes || !a_port.FrameSampled())
				return false;
			if (pass & EffectFlag::Membrane)
				return false;
			a_port.ResetRecord(kMaterialKindEffect);

			const auto& vs = EffectVSLayout();
			const auto& ps = EffectPSLayout();
			float* const vsFloats = a_port.VSFloats();
			float* const psFloats = a_port.PSFloats();
			const auto bits = [&](std::size_t a_offset) { return a_port.At<std::uint32_t>(a_offset); };
			const auto setPS = [&](std::uint32_t a_variable, std::uint32_t a_component, float a_value) {
				psFloats[ps.offset[a_variable] + a_component] = a_value;
			};
			const auto copyVS = [&](std::uint32_t a_variable, std::uint32_t a_component, std::size_t a_offset) {
				const std::uint32_t value = bits(a_offset);
				std::memcpy(&vsFloats[vs.offset[a_variable] + a_component], &value, sizeof(value));
			};
			const auto copyPS = [&](std::uint32_t a_variable, std::uint32_t a_component, std::size_t a_offset) {
				const std::uint32_t value = bits(a_offset);
				std::memcpy(&psFloats[ps.offset[a_variable] + a_component], &value, sizeof(value));
			};

			if (pass & EffectFlag::Texture) {
				a_port.SetFilterMode(0, kFilterAnisotropic);
				if (a_port.HasTexture(kSourceTexture))
					a_port.BindTexture(0, a_port.View(kSourceTexture), a_port.At<std::uint8_t>(kClampMode));
				const std::size_t buffer = std::size_t(a_port.TextureTransformBuffer() & 1u) * 8;
				copyVS(kEffectVSTexcoordOffset, 0, kTexcoordOffset + buffer);
				copyVS(kEffectVSTexcoordOffset, 1, kTexcoordOffset + 4 + buffer);
				copyVS(kEffectVSTexcoordOffset, 2, kTexcoordScale + buffer);
				copyVS(kEffectVSTexcoordOffset, 3, kTexcoordScale + 4 + buffer);
			}
			if (pass & (EffectFlag::GrayscaleToColor | EffectFlag::GrayscaleToAlpha)) {
				// The engine dereferences the palette without a null check, so DCLF never draws a pass without one.
				if (!a_port.HasTexture(kGreyscaleTexture))
					return false;
				a_port.BindTexture(4, a_port.View(kGreyscaleTexture), 0);
				a_port.SetFilterMode(4, kFilterBilinear);
			}

			// With GrayscaleToColor the scale stays separate because the palette needs it.
			const float scale = a_port.At<float>(kBaseColorScale);
			if (!(pass & EffectFlag::GrayscaleToColor)) {
				setPS(kEffectPSBaseColor, 0, scale * a_port.At<float>(kBaseColor));
				setPS(kEffectPSBaseColor, 1, a_port.At<float>(kBaseColor + 4) * scale);
				setPS(kEffectPSBaseColor, 2, a_port.At<float>(kBaseColor + 8) * scale);
				copyPS(kEffectPSBaseColor, 3, kBaseColor + 12);
				// Linear Lighting's EffectMaterialUpload: the rgb in the working space, then the scale, except for Blood.
				if (const auto* space = Integrations::LinearLighting::FrameWorkingSpace(a_port.frame.contributors); space && !(pass & EffectFlag::Blood)) {
					float colour[3] = { a_port.At<float>(kBaseColor), a_port.At<float>(kBaseColor + 4), a_port.At<float>(kBaseColor + 8) };
					Integrations::LinearLighting::ToWorking(*space, colour);
					for (std::uint32_t c = 0; c < 3; ++c)
						setPS(kEffectPSBaseColor, c, colour[c] * scale);
				}
			} else {
				for (std::uint32_t c = 0; c < 4; ++c)
					copyPS(kEffectPSBaseColor, c, kBaseColor + 4 * c);
				copyPS(kEffectPSBaseColorScale, 0, kBaseColorScale);
			}
			setPS(kEffectPSLightingInfluence, 0, static_cast<float>(a_port.At<std::uint8_t>(kLightingInfluence)) * std::bit_cast<float>(kInverse255Bits));
			if (pass & EffectFlag::Soft) {
				// The engine writes near * far / softFalloffDepth. DCLF stores the depth and the shader divides, so the record
				// does not change with the camera.
				copyPS(kEffectPSLightingInfluence, 1, kSoftFalloffDepth);
				copyVS(kEffectVSSoftMaterialParams, 0, kSoftFalloffDepth);
			}
			if (pass & EffectFlag::Falloff)
				for (std::uint32_t c = 0; c < 4; ++c)
					copyVS(kEffectVSFalloffData, c, kFalloff + 4 * c);
			return true;
		}

		void DeriveEffect(Api::MaterialPortDerivation& a_derivation)
		{
			a_derivation.reason = DeriveEffectDescriptors(a_derivation.property, a_derivation.leaf, *a_derivation.out);
		}

		std::uint32_t EffectVariants(const Api::MaterialPortDerivation& a_derivation)
		{
			return EffectVariantsOf(a_derivation.property, a_derivation.leaf);
		}

		bool EvaluateEffectPort(Api::MaterialPortEvaluation& a_evaluation)
		{
			// Community Shaders' only hook on BSEffectShader's SetupMaterial is Linear Lighting's, which EvaluateEffect ports.
			return EvaluateEffect(a_evaluation);
		}
	}

	void RegisterPort(Api::InstallContext& a_context)
	{
		// Must be the first port registered so that it gets kind 1.
		static_assert(kMaterialKindEffect == 1);
		static const Api::MaterialPortContribution port{
			.name = "effect",
			.shaderType = static_cast<std::uint8_t>(RE::BSShader::Type::Effect),
			.objectClass = Api::ObjectClass::Effect,
			.propertyRtti = []() -> const void* { return REL::Relocation<const RE::NiRTTI*>{ RE::BSEffectShaderProperty::Ni_RTTI }.get(); },
			.materialVtable = []() -> const void* { return reinterpret_cast<const void*>(RE::VTABLE_BSEffectShaderMaterial[0].address()); },
			.materialBytes = kEffectMaterialBytes,
			.textureFields = kEffectTextureFields,
			.vs = { .variables = kEffectVSVariables, .sized = kEffectVSSized, .scratch = kEffectVSScratch,
				.groups = { kEffectVSGroups[0], kEffectVSGroups[1], kEffectVSGroups[2] },
				.first = { kEffectVSFirstVariable[0], kEffectVSFirstVariable[1], kEffectVSFirstVariable[2] }, .names = kEffectVSNames },
			.ps = { .variables = kEffectPSVariables, .groups = { kEffectPSGroups[0], kEffectPSGroups[1], kEffectPSGroups[2] },
				.first = { kEffectPSFirstVariable[0], kEffectPSFirstVariable[1], kEffectPSFirstVariable[2] }, .names = kEffectPSNames },
			.rules = { .passEnumBase = kEffectPassEnumBase, .deferredPixelBit = EffectFlag::Deferred, .projectedPassBit = 0, .frameVSVariable = kEffectVSTexcoordOffset,
				.techniqueRows = false, .rawPermutation = true, .signatureComponents = false, .emissiveAlwaysListed = true, .turnedByBillboards = true,
				.particleGeometry = true },
			// Features/TextureTransforms patches these rows every frame.
			.frameVariables = kEffectFrameVariables,
			.capture = &CaptureEffectProperty,
			.derive = &DeriveEffect,
			.evaluate = &EvaluateEffectPort,
			.shading = &SampleEffectShading,
			.variants = &EffectVariants,
			.variantNames = kEffectVariantNames,
			.emissiveOf = &EffectEmissive,
		};
		if (a_context.RegisterMaterialPort(port) != kMaterialKindEffect)
			::RenderGraph::Fatal("Drawcall Limit Fix", "the Effect port did not take material kind 1 (a feature before Effects registered a port)");
	}
}

// The Effect port's property capture and shading row. Pure except for engine reads, made where Core holds the property.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectDescriptors.h"

#include <algorithm>

namespace DCLF::Features::Effects
{
	void CaptureEffectProperty(const Api::MaterialPortCapture& a_capture)
	{
		// Read here while the property holds the material. GetRenderPasses tests a texture path by its string pointer, not the texture.
		a_capture.emissive = static_cast<const RE::BSEffectShaderProperty&>(a_capture.property).emittanceColor;
		if (const auto* material = a_capture.property.material) {
			const auto* bytes = reinterpret_cast<const std::byte*>(material);
			const auto pathAt = [&](std::size_t a_offset) { return *reinterpret_cast<const char* const*>(bytes + a_offset) != nullptr; };
			a_capture.feature = (pathAt(0x70) ? kEffectHasSourcePath : 0u) | (pathAt(0x78) ? kEffectHasGreyscalePath : 0u);
		}
	}

	const void* EffectEmissive(const RE::BSShaderProperty& a_property)
	{
		// FUN_141480fc0 re-points emittanceColor as it does a Lighting property's emissiveColor.
		return static_cast<const RE::BSEffectShaderProperty&>(a_property).emittanceColor;
	}

	void SampleEffectShading(Api::MaterialPortShading& a_shading)
	{
		// As SetupGeometry (0x1414eab10) writes PropertyColor. Alpha is the material's base alpha times the fade node's currentFade.
		const auto& effect = static_cast<const RE::BSEffectShaderProperty&>(a_shading.property);
		const auto* material = static_cast<const RE::BSEffectShaderMaterial*>(effect.material);
		auto& row = a_shading.out;
		auto& shading = row.shading;
		shading.materialData[0] = shading.materialData[1] = 0.0f;
		shading.materialData[2] = a_shading.member && material ? material->baseColor.alpha : effect.alpha;
		// 1 picks the cloud light, as FUN_1414fcf80 does for a kCloudLOD property. 0 picks the sun.
		const bool cloudLight = Ported().cloudLighting && effect.flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kCloudLOD);
		shading.materialData[3] = cloudLight ? 1.0f : 0.0f;
		const auto* emittance = effect.emittanceColor;
		shading.emitColor[0] = emittance ? emittance->red : 1.0f;
		shading.emitColor[1] = emittance ? emittance->green : 1.0f;
		shading.emitColor[2] = emittance ? emittance->blue : 1.0f;
		shading.ssrSpecular = 0.0f;
		row.written = 4u | 0x70u;  // MaterialData.w is DCLF-only, with no engine write to compare
		row.padding[0] = row.padding[1] = row.padding[2] = 0u;
		std::fill(std::begin(row.featureBlock), std::end(row.featureBlock), 0.0f);
	}
}

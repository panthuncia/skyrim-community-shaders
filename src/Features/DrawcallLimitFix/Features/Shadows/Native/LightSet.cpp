// The shadow light set's values and its hand-over from the game side to the render side (see LightSet.h). The capture itself is
// NativeCasters.cpp's, next to the selection.

#include "Features/DrawcallLimitFix/Features/Shadows/Native/LightSet.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "RenderGraph/FrameAnchors.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace DCLF::Features::Shadows::Native
{
	namespace
	{
		namespace Address
		{
			// The shadow settings the masks read (AE, SE).
			inline constexpr DCLF::Engine::Offset kSceneShadows{ { 390933 }, { 513204 } };     // bool: the scene's shadows are on
			inline constexpr DCLF::Engine::Offset kFilterMode{ { 390942 }, { 513194 } };       // std::uint32_t: the shadow filter; 2 and 3 take the Poisson radius
			inline constexpr DCLF::Engine::Offset kFilterBits{ { 390941 }, { 513193 } };       // std::uint32_t: the filter's descriptor bits; 0 samples t3
			inline constexpr DCLF::Engine::Offset kPoissonRadius{ { 391759 }, { 513773 } };    // float
			inline constexpr DCLF::Engine::Offset kMapResolution{ { 415268 }, { 528318 } };    // std::uint32_t
			inline constexpr DCLF::Engine::Offset kFarParam{ { 415264 }, { 528316 } };         // float: ShadowLightParam.z for a light with +0x63 set
			inline constexpr DCLF::Engine::Offset kSunBiasScale{ { 391869 }, { 513809 } };     // float: AlphaTestRef.y's factor for a cascaded light
			inline constexpr DCLF::Engine::Offset kSecondBiasScale{ { 391716 }, { 513749 } };  // float: AlphaTestRef.z's factor
			inline constexpr DCLF::Engine::Offset kBiasScale{ { 415265 }, { 528317 } };        // float: both biases' last factor
		}

		// BSShadowLight fields past CommonLib's.
		constexpr std::size_t kFarFlag = 0x63;         // bool
		constexpr std::size_t kSpotFalloff = 0x568;    // float: BSShadowFrustumLight's falloff
		constexpr std::size_t kSunSplitStart = 0x598;  // float[3]: BSShadowDirectionalLight's cascade starts
		constexpr std::size_t kSunSplitEnd = 0x5A4;    // float[3]: and ends

		template <class T>
		T Field(const void* a_object, std::size_t a_offset)
		{
			T value;
			std::memcpy(&value, static_cast<const std::byte*>(a_object) + a_offset, sizeof(T));
			return value;
		}

		// Game side: the last set made. Render side: the set the render half adopted.
		std::shared_ptr<const LightSet> made;
		std::shared_ptr<const LightSet> adopted;
	}

	const ShadowLight* LightSet::Find(const RE::BSShadowLight* a_light) const
	{
		const auto found = std::ranges::find(lights, a_light, &ShadowLight::light);
		return found != lights.end() ? &*found : nullptr;
	}

	LightKind LightKindOf(const RE::BSShadowLight& a_light)
	{
		auto& light = const_cast<RE::BSShadowLight&>(a_light);
		return light.GetIsDirectionalLight() ? LightKind::Directional : light.GetIsParabolicLight() ? LightKind::Parabolic :
		                                                                                              LightKind::Frustum;
	}

	ShadowLight ReadLight(const RE::BSShadowLight& a_light)
	{
		auto& light = const_cast<RE::BSShadowLight&>(a_light);
		ShadowLight out;
		out.light = &a_light;
		out.kind = LightKindOf(a_light);
		out.omni = light.GetIsOmniLight();
		out.maps = light.shadowMapCount;
		const auto& data = light.GetRuntimeData();
		out.maskIndex = data.maskIndex;
		out.drawFocusShadows = data.drawFocusShadows;
		out.biasScale = data.shadowBiasScale;
		out.farFlag = Field<bool>(&a_light, kFarFlag);
		out.affectsLand = a_light.affectLand;
		out.lodDimmer = a_light.lodDimmer;
		if (out.kind == LightKind::Frustum && light.GetIsFrustumLight())
			out.spotFalloff = Field<float>(&a_light, kSpotFalloff);
		if (const auto* niLight = a_light.light.get()) {
			out.hasNiLight = true;
			out.hidden = niLight->GetFlags().any(RE::NiAVObject::Flag::kHidden);
			const auto& lightData = const_cast<RE::NiLight*>(niLight)->GetLightRuntimeData();
			out.position = { niLight->world.translate.x, niLight->world.translate.y, niLight->world.translate.z };
			out.diffuse = { lightData.diffuse.red, lightData.diffuse.green, lightData.diffuse.blue };
			out.radius = lightData.radius.x;
			out.fade = lightData.fade;
		}
		out.descriptors = ReadDescriptors(a_light);
		return out;
	}

	std::vector<MapValues> ReadDescriptors(const RE::BSShadowLight& a_light)
	{
		const auto& descriptors = const_cast<RE::BSShadowLight&>(a_light).GetRuntimeData().shadowmapDescriptors;
		std::vector<MapValues> out;
		out.reserve(descriptors.size());
		for (const auto& descriptor : descriptors) {
			auto& values = out.emplace_back();
			values.target = static_cast<std::int32_t>(descriptor.renderTarget);
			values.slice = descriptor.shadowmapIndex;
			static_assert(sizeof(values.lightTransform) == sizeof(descriptor.lightTransform));
			std::memcpy(values.lightTransform.data(), &descriptor.lightTransform, sizeof(values.lightTransform));
			values.culls = descriptor.cullingProcess && descriptor.isEnabled;
		}
		return out;
	}

	std::vector<ShadowLight> ReadLights(const RE::ShadowSceneNode* a_node)
	{
		std::vector<ShadowLight> lights;
		if (!a_node)
			return lights;
		const auto& casters = const_cast<RE::ShadowSceneNode*>(a_node)->GetRuntimeData().shadowLightsAccum;
		// The masks' walk: each light takes as many slots as it has maps. A light with none ends it (the masks refuse it).
		for (std::uint32_t slot = 0; slot < casters.size();) {
			const auto* light = casters[slot];
			if (!light)
				break;
			lights.push_back(ReadLight(*light));
			if (!light->shadowMapCount)
				break;
			slot += light->shadowMapCount;
		}
		return lights;
	}

	MaskSettings ReadMaskSettings()
	{
		MaskSettings out;
		out.filterMode = DCLF::Engine::Global<std::uint32_t>(Address::kFilterMode);
		out.filterBits = DCLF::Engine::Global<std::uint32_t>(Address::kFilterBits);
		out.sceneShadows = DCLF::Engine::Global<bool>(Address::kSceneShadows);
		out.focusTargets = out.sceneShadows && DCLF::Engine::Global<std::uint32_t>(DCLF::Engine::Address::kFocusTargetCount) != 0;
		out.poisson = DCLF::Engine::Global<float>(Address::kPoissonRadius);
		out.resolution = static_cast<float>(DCLF::Engine::Global<std::uint32_t>(Address::kMapResolution));
		out.farParam = DCLF::Engine::Global<float>(Address::kFarParam);
		out.biasScale = DCLF::Engine::Global<float>(Address::kBiasScale);
		out.sunBiasScale = DCLF::Engine::Global<float>(Address::kSunBiasScale);
		out.secondBiasScale = DCLF::Engine::Global<float>(Address::kSecondBiasScale);
		return out;
	}

	SunSplits ReadSun(const RE::BSShadowLight* a_sun)
	{
		SunSplits out;
		if (!a_sun)
			return out;
		out.light = a_sun;
		out.maps = a_sun->shadowMapCount;
		for (std::uint32_t i = 0; i < (std::min)(out.maps, 3u); ++i) {
			out.start[i] = Field<float>(a_sun, kSunSplitStart + i * 4);
			out.end[i] = Field<float>(a_sun, kSunSplitEnd + i * 4);
		}
		return out;
	}

	void MadeSet(std::shared_ptr<const LightSet> a_set)
	{
		made = std::move(a_set);
	}

	std::shared_ptr<const LightSet> LastSet()
	{
		return made;
	}

	void AdoptSet(std::shared_ptr<const LightSet> a_set)
	{
		adopted = std::move(a_set);
	}

	const LightSet* RenderSet()
	{
		return adopted.get();
	}

	const CasterBoards* AdoptedBoards()
	{
		if (Scene::FrameView::InGameHalf() || !adopted || !adopted->boards.views || !::FrameAnchors::IsRenderOwner())
			return nullptr;
		return &adopted->boards;
	}
}

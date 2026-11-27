// Samples the local shadow lights from DCLF's selection and light views. Render thread. Native reads the lights' values from the shadow
// light set (Shadows/Native/LightSet.h); guest reads the lights, which its render thread draws.

#include "Features/DrawcallLimitFix/Features/LocalLights/LocalShadowLights.h"
#include "Snapshot/EngineReads.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LocalLights.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/LightSet.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		using Features::Shadows::Native::ShadowLight;

		/** @brief a_light's values: the light set's in native (the set the render half adopted), else read now. Null when it is not there. */
		const ShadowLight* ValuesOf(const RE::BSShadowLight* a_light, ShadowLight& a_read)
		{
			if (const auto* set = Features::Shadows::Native::RenderSet())
				return set->Find(a_light);
			a_read = Features::Shadows::Native::ReadLight(*a_light);
			return &a_read;
		}
	}

	LocalShadowLights LocalShadowLights::Sample()
	{
		LocalShadowLights out;
		// Each light appears once. Its volumes are the planes each descriptor's cull tests (Process2).
		const std::uint32_t sceneFrame = Scene::FrameView::Get().SceneStep();
		const auto* selection = LightSelection::Get().Current(sceneFrame);
		if (!selection)
			return out;
		for (const auto& selected : selection->locals) {
			if (selected.light == selection->sun)
				continue;
			ShadowLight read;
			const auto* values = ValuesOf(selected.light, read);
			if (!values)
				continue;
			Light sampled;
			sampled.maskBit = 1u << (selected.maskIndex & 31);
			sampled.affectsLand = values->affectsLand;
			// Gated only when the atlas holding its context was encoded.
			if (selected.portalContext && selected.portalContextIndex != ~0u && !selection->portalContexts.empty())
				sampled.portalContext = selected.portalContextIndex + 1;
			if (values->hasNiLight) {
				sampled.center[0] = values->position[0];
				sampled.center[1] = values->position[1];
				sampled.center[2] = values->position[2];
				sampled.radius = values->radius;
			}
			const auto source = values->kind == Features::Shadows::Native::LightKind::Parabolic ? Api::ShadowSource::Point : Api::ShadowSource::Spot;
			for (std::uint32_t d = 0; d < values->descriptors.size(); ++d) {
				if (!values->descriptors[d].culls)
					continue;
				Api::ShadowSourceCamera request{ selected.light, d, sceneFrame };
				const auto* view = Extensions::SourceCamera(source, request);
				if (!view)
					continue;
				Light::Volume volume;
				for (std::uint32_t q = 0; q < 6; ++q)
					volume.planes[q] = view->cull.plane[q];
				volume.masks[0] = view->cull.mask & 0x3Fu;
				sampled.volumes.push_back(volume);
			}
			out.lights.push_back(std::move(sampled));
		}
		return out;
	}

	std::uint32_t LocalShadowLights::SamplePointLights(Api::PointLight* a_out, std::uint32_t a_capacity)
	{
		std::uint32_t count = 0;
		// A shadow light's values come from the light set in native.
		const auto putShadow = [&](const RE::BSShadowLight* a_light, std::uint32_t a_maskIndex) {
			ShadowLight read;
			const auto* values = ValuesOf(a_light, read);
			if (count >= a_capacity || !values || !values->hasNiLight || values->hidden)
				return;
			auto& out = a_out[count++];
			out.position[0] = values->position[0];
			out.position[1] = values->position[1];
			out.position[2] = values->position[2];
			out.radius = values->radius;
			const float scale = values->fade * values->lodDimmer;
			out.color[0] = scale * values->diffuse[0];
			out.color[1] = scale * values->diffuse[1];
			out.color[2] = scale * values->diffuse[2];
			out.shadowMaskIndex = a_maskIndex;
			out.affectsLand = values->affectsLand;
		};
		const auto put = [&](const RE::BSLight* a_light, std::uint32_t a_maskIndex) {
			const auto* niLight = a_light ? a_light->light.get() : nullptr;
			if (count >= a_capacity || !niLight || niLight->GetFlags().any(RE::NiAVObject::Flag::kHidden))
				return;
			const auto& data = niLight->GetLightRuntimeData();
			auto& out = a_out[count++];
			out.position[0] = niLight->world.translate.x;
			out.position[1] = niLight->world.translate.y;
			out.position[2] = niLight->world.translate.z;
			out.radius = data.radius.x;
			// Diffuse times the NiLight's fade times the BSLight's lodDimmer (+0x14).
			const float scale = data.fade * a_light->lodDimmer;
			out.color[0] = scale * data.diffuse.red;
			out.color[1] = scale * data.diffuse.green;
			out.color[2] = scale * data.diffuse.blue;
			out.shadowMaskIndex = a_maskIndex;
			out.affectsLand = a_light->affectLand;
		};
		if (const auto* selection = LightSelection::Get().Current(Scene::FrameView::Get().SceneStep()))
			for (const auto& selected : selection->locals)
				if (selected.light != selection->sun)
					putShadow(selected.light, selected.maskIndex);
		// ShadowSceneNode's active lights (+0x130).
		auto* sceneNode = CS_ENGINE_READ(ShaderManagerState, CapturePoint, globals::game::smState) ? CS_ENGINE_READ(ShaderManagerState, CapturePoint, globals::game::smState)->shadowSceneNode[0] : nullptr;
		if (sceneNode)
			for (const auto& entry : sceneNode->GetRuntimeData().activeLights)
				if (const auto* light = entry.get(); light && light->frustrumCull != 0xFFu)
					put(light, ~0u);
		return count;
	}
}

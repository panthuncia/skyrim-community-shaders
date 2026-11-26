#include "Snapshot/CoreFrame.h"

#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"

#include "Features/LinearLighting.h"
#include "Features/SkySync.h"
#include "Globals.h"
#include "Utils/Game.h"
#include "Utils/Moon.h"

namespace Snapshot
{
	namespace
	{
		/** @brief The sun light and the DALC, from the shadow scene node and the shader manager. */
		void CaptureLight(CoreFrame& a_core)
		{
			const auto* shaderManager = CS_ENGINE_READ(ShaderManagerState, CapturePoint, globals::game::smState);
			const auto& transform = shaderManager->directionalAmbientTransform;
			for (int row = 0; row < 3; ++row)
				for (int column = 0; column < 3; ++column)
					a_core.dalc[row * 3 + column] = transform.rotate.entry[row][column];
			a_core.dalc[9] = transform.translate.x;
			a_core.dalc[10] = transform.translate.y;
			a_core.dalc[11] = transform.translate.z;

			auto* shadowSceneNode = shaderManager->shadowSceneNode[0];
			const auto* light = skyrim_cast<RE::NiDirectionalLight*>(shadowSceneNode->GetRuntimeData().sunLight->light.get());
			const auto& lightData = light->GetLightRuntimeData();
			const auto& linearLighting = globals::features::linearLighting;
			a_core.dirLightColor = lightData.diffuse;
			a_core.dirLightFade = lightData.fade;
			a_core.dirLightDirection = light->GetWorldDirection();
			a_core.dirLightOverridden = linearLighting.IsSunlightOverridden(light);
			a_core.sunlightScale = linearLighting.GetSunlightScale(light);
		}

		/** @brief The sky's flags, the sun and the moons. Moon colours use Sky Sync's phase settings. */
		void CaptureSky(CoreFrame& a_core)
		{
			const auto* sky = CS_ENGINE_READ(Sky, CapturePoint, globals::game::sky);
			if (!sky)
				return;
			a_core.hasSky = true;
			a_core.hideSky = sky->flags.any(RE::Sky::Flags::kHideSky);
			const auto& skySync = globals::features::skySync;
			const auto toFloat4 = [](const RE::NiPoint3& a_dir) { return float4{ a_dir.x, a_dir.y, a_dir.z, 0.0f }; };
			const auto moonColor = [&](const RE::Moon* a_moon, const Util::ColorSpace::LightColor& a_base) {
				return Util::Moon::GetLightColor(a_moon, a_base, skySync.settings.NewMoonIntensity, skySync.settings.CrescentMoonIntensity, skySync.settings.FullMoonIntensity);
			};

			if (const auto* sun = sky->sun; sun && sun->root && sky->root) {
				a_core.sunDirection = toFloat4(skySync.GetCelestialDirection(sky, SkySync::Caster::Sun));
				if (sun->sunBase)
					if (const auto* property = skyrim_cast<RE::BSSkyShaderProperty*>(sun->sunBase->GetGeometryRuntimeData().shaderProperty.get())) {
						const auto& color = property->kBlendColor;
						a_core.sun = { .color = { color.red, color.green, color.blue }, .intensity = color.alpha, .alpha = color.alpha };
					}
			}
			if (const auto* masser = sky->masser) {
				a_core.masserDirection = toFloat4(skySync.GetCelestialDirection(sky, SkySync::Caster::Masser));
				if (masser->root && !masser->root->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					a_core.masser = moonColor(masser, Util::Moon::MasserBaseColor);
			}
			// Bottled Shaders keeps cloud layer 28 out of the scattering.
			constexpr std::uint16_t kNoScatteringCloudLayer = 28;
			if (sky->clouds && kNoScatteringCloudLayer < sky->clouds->numLayers)
				a_core.noScatteringCloud = reinterpret_cast<std::uintptr_t>(sky->clouds->clouds[kNoScatteringCloudLayer].get());
			if (const auto* secunda = sky->secunda) {
				a_core.secundaMoonMesh = reinterpret_cast<std::uintptr_t>(secunda->moonMesh.get());
				a_core.secundaShadowMesh = reinterpret_cast<std::uintptr_t>(secunda->shadowMesh.get());
				a_core.secundaDirection = toFloat4(skySync.GetCelestialDirection(sky, SkySync::Caster::Secunda));
				if (secunda->root && !secunda->root->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					a_core.secunda = moonColor(secunda, Util::Moon::SecundaBaseColor);
			}
		}
	}

	void CaptureCore(Builder& a_builder)
	{
		CoreFrame core{};
		const auto camera = Util::GetCameraData();
		core.cameraFar = camera.x;
		core.cameraNear = camera.y;
		core.fovDegrees = Util::GetWorldFOVDegrees();
		core.noSky = Util::IsInterior();
		CaptureLight(core);
		CaptureSky(core);
		// The main camera's position is the eye the main view renders from (BSGraphics posAdjust).
		if (const auto& main = a_builder.Built().cameras.Current(ViewId::Main); main.valid)
			for (int i = -2; i <= 2; ++i)
				for (int k = -2; k <= 2; ++k)
					core.water[(i + 2) + (k + 2) * 5] = Util::TryGetWaterData(main.position, static_cast<float>(i) * 4096.0f, static_cast<float>(k) * 4096.0f);
		else
			core.water.fill(float4(1.0f, 1.0f, 1.0f, -FLT_MAX));
		a_builder.Set(core);
	}

	const CoreFrame& Core()
	{
		static const CoreFrame empty{};
		const auto& snapshot = Current();
		const auto* core = snapshot ? snapshot->Block<CoreFrame>() : nullptr;
		return core ? *core : empty;
	}
}

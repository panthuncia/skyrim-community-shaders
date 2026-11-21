// LocalLights feature registration: the light selection and the light volumes.

#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LightContextResources.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LocalLights.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LocalShadowLights.h"

#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LightVolumes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"

#include <cstring>

namespace DCLF::Features::LocalLights
{
	namespace
	{
		// Installed from the Spot source's hooks, to keep the install log in engine order.
		void SourceHooks(Api::ShadowSourceHooks& a_hooks)
		{
			if (a_hooks.source == Api::ShadowSource::Spot)
				LightSelection::Get().Install();
		}

		Api::ClassifyInputId lightLimitFix;

		// Without Light Limit Fix the Lighting draws use the frame's point light table. This is fixed for the session.
		bool PointLightsActive()
		{
			return !Extensions::ClassifyInputOn(lightLimitFix);
		}

		void Sample(Api::LightVolumeSink& a_sink)
		{
			const auto localShadows = LocalShadowLights::Sample();
			for (const auto& light : localShadows.lights) {
				for (const auto& volume : light.volumes) {
					Api::LightVolume out;
					out.masks[0] = volume.masks[0];
					out.masks[1] = volume.masks[1];
					out.maskBit = light.maskBit;
					out.affectsLand = light.affectsLand;
					out.sphere[0] = light.center[0];
					out.sphere[1] = light.center[1];
					out.sphere[2] = light.center[2];
					out.sphere[3] = light.radius;
					std::memcpy(out.planes, volume.planes.data(), sizeof(out.planes));
					out.portalContext = light.portalContext;
					a_sink.Put(out);
				}
			}
		}

		void Install(Api::InstallContext& a_context)
		{
			RegisterLightContexts(a_context);
			// Landscape properties take only the lights that affect land.
			lightLimitFix = a_context.NeedClassifyInput("light limit fix");
			a_context.RegisterLightVolumes({ .name = "local shadow lights",
				.landscapeFlags = kLandscapeFlags,
				.sample = &Sample,
				.pointLightsActive = &PointLightsActive,
				.pointLights = &LocalShadowLights::SamplePointLights });
		}
	}

	Api::Feature Feature()
	{
		return { .name = "local lights", .install = &Install, .sourceHooks = &SourceHooks };
	}
}

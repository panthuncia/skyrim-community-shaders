// The one ordered list of DCLF's integrations: CS features DCLF supports, written against Api/ like features.
// They install after every feature, so they may name what a feature registered. List order is registration order.

#include "Features/DrawcallLimitFix/Integrations/IntegrationList.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"
#include "Features/DrawcallLimitFix/Integrations/CloudShadows/CloudShadows.h"
#include "Features/DrawcallLimitFix/Integrations/DynamicCubemaps/DynamicCubemaps.h"
#include "Features/DrawcallLimitFix/Integrations/ExponentialHeightFog/ExponentialHeightFog.h"
#include "Features/DrawcallLimitFix/Integrations/ExtendedTranslucency/ExtendedTranslucency.h"
#include "Features/DrawcallLimitFix/Integrations/IBL/IBL.h"
#include "Features/DrawcallLimitFix/Integrations/ImprovedCamera/ImprovedCamera.h"
#include "Features/DrawcallLimitFix/Integrations/LightLimitFix/LightLimitFix.h"
#include "Features/DrawcallLimitFix/Integrations/LinearLighting/LinearLighting.h"
#include "Features/DrawcallLimitFix/Integrations/PhysicalSky/PhysicalSky.h"
#include "Features/DrawcallLimitFix/Integrations/ScreenSpaceShadows/ScreenSpaceShadows.h"
#include "Features/DrawcallLimitFix/Integrations/Skylighting/Skylighting.h"
#include "Features/DrawcallLimitFix/Integrations/SubsurfaceScattering/SubsurfaceScattering.h"
#include "Features/DrawcallLimitFix/Integrations/TerrainBlending/TerrainBlending.h"
#include "Features/DrawcallLimitFix/Integrations/TerrainShadows/TerrainShadows.h"
#include "Features/DrawcallLimitFix/Integrations/TruePBR/TruePBRIntegration.h"
#include "Features/DrawcallLimitFix/Integrations/ViewAtmosphere/ViewAtmosphere.h"
#include "Features/DrawcallLimitFix/Integrations/VolumetricShadows/VolumetricShadows.h"
#include "Features/DrawcallLimitFix/Integrations/WaterEffects/WaterEffects.h"

#include <array>

namespace DCLF::Integrations
{
	std::span<const Api::Feature> List()
	{
		static const std::array kIntegrations{
			AdvancedSkin::Integration(),
			TerrainBlending::Integration(),
			LightLimitFix::Integration(),
			TruePBR::Integration(),
			LinearLighting::Integration(),
			SubsurfaceScattering::Integration(),
			ExtendedTranslucency::Integration(),
			VolumetricShadows::Integration(),
			ExponentialHeightFog::Integration(),
			ViewAtmosphere::Integration(),
			CloudShadows::Integration(),
			DynamicCubemaps::Integration(),
			Skylighting::Integration(),
			TerrainShadows::Integration(),
			WaterEffects::Integration(),
			IBL::Integration(),
			PhysicalSky::Integration(),
			ScreenSpaceShadows::Integration(),
			ImprovedCamera::Integration(),
		};
		return kIntegrations;
	}

	std::vector<std::string> StatusLines()
	{
		std::vector<std::string> lines;
		for (const auto& integration : List())
			lines.push_back(fmt::format("{}: {}", integration.name, integration.status ? integration.status() : std::string("installed")));
		return lines;
	}
}

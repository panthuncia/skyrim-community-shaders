#include "Common/Game.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/LightingParity.hlsli"

namespace InverseSquareLighting
{
	static const float SCALE = 0.8f;
	static const float METRES_TO_UNITS_SQ = METRES_TO_UNITS * METRES_TO_UNITS;
	static const float SCALED_UNITS_SQ = SCALE * METRES_TO_UNITS_SQ;

	float GetAttenuation(float distance, LightLimitFix::Light light)
	{
		return CSLightingAttenuation(distance, light.radius, light.invRadius,
			light.fadeZone, light.sizeBias, SCALED_UNITS_SQ,
			(light.lightFlags & LightLimitFix::LightFlags::Disabled) != 0,
			(light.lightFlags & LightLimitFix::LightFlags::InverseSquare) != 0);
	}
}

#ifndef CS_DEFERRED_LIGHTING_COMMON_HLSLI
#define CS_DEFERRED_LIGHTING_COMMON_HLSLI

#include "../Common/LightingParity.hlsli"
#include "DeferredMaterial.hlsli"

// Mechanical helpers shared by the D3D12 deferred passes. Visual lighting
// equations remain owned by Community Shaders and will move here only after
// the corresponding D3D11 geometry helper has been parameterized and tested.
static const uint CS_DEFERRED_ABI_VERSION = 4u;
static const uint CS_NULL_LIGHT_PAGE = 0xFFFFFFFFu;
static const uint CS_LIGHT_FLAG_PORTAL_STRICT = 1u << 0;
static const uint CS_LIGHT_FLAG_SHADOW = 1u << 1;
static const uint CS_CONTEXT_RECEIVES_DEFERRED_SHADOW = 1u << 2;
static const uint CS_CONTEXT_RECEIVES_DIRECTIONAL_SHADOW = 1u << 3;
static const uint CS_CONTEXT_USES_CHARACTER_LIGHT = 1u << 4;

float CSDeferredAttenuation(float distanceToLight, Light light)
{
	return CSLightingAttenuation(distanceToLight, light.radius, light.invRadius,
		light.fadeZone, light.sizeBias, 0.8f * METRES_TO_UNITS * METRES_TO_UNITS,
		(light.lightFlags & (1u << 9)) != 0,
		(light.lightFlags & (1u << 10)) != 0);
}

float3 CSDeferredTransformLight(float3 color, bool isLinear, float gamma, float multiplier, bool linearLighting)
{
	// Color::DirectionalLight/PointLight compensate traditional (non-linear)
	// Lambert lighting by PI. Together with VanillaNormalization this is part of
	// the existing CS visual contract, not an optional energy correction.
	return CSLightingTransformLight(color, isLinear, linearLighting, gamma, multiplier,
		linearLighting ? 1.0f : 3.14159265358979323846f);
}

float3 CSDeferredAmbient(LightingContext context, float3 normal, bool linearLighting,
	float ambientGamma, float ambientMultiplier)
{
	return CSLightingDirectionalAmbient(context.directionalAmbient[0],
		context.directionalAmbient[1], context.directionalAmbient[2], normal,
		linearLighting, ambientGamma, ambientMultiplier);
}

float3 CSDeferredIrradianceToGamma(float3 irradiance, bool linearLighting)
{
	return CSLightingIrradianceToGamma(irradiance, linearLighting);
}

#endif

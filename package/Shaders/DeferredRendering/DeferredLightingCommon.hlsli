#ifndef CS_DEFERRED_LIGHTING_COMMON_HLSLI
#define CS_DEFERRED_LIGHTING_COMMON_HLSLI

#include "../Common/LightingParity.hlsli"

// Mechanical helpers shared by the D3D12 deferred passes. Visual lighting
// equations remain owned by Community Shaders and will move here only after
// the corresponding D3D11 geometry helper has been parameterized and tested.
static const uint CS_DEFERRED_ABI_VERSION = 1u;
static const uint CS_INVALID_CONTEXT = 0xFFFFu;
static const uint CS_LEGACY_MATERIAL = 0u;
static const uint CS_NULL_LIGHT_PAGE = 0xFFFFFFFFu;
static const uint CS_LIGHT_FLAG_PORTAL_STRICT = 1u << 0;
static const uint CS_LIGHT_FLAG_SHADOW = 1u << 1;
static const uint CS_CONTEXT_RECEIVES_DEFERRED_SHADOW = 1u << 2;
static const uint CS_CONTEXT_RECEIVES_DIRECTIONAL_SHADOW = 1u << 3;
static const uint CS_CONTEXT_USES_CHARACTER_LIGHT = 1u << 4;

uint CSDeferredContextIndex(uint packedSurface)
{
	return packedSurface & 0xFFFFu;
}

uint CSDeferredMaterialClass(uint packedSurface)
{
	return (packedSurface >> 16u) & 0xFFu;
}

float CSDeferredAttenuation(float distanceToLight, Light light)
{
	float enabled = float((light.lightFlags & (1u << 9)) == 0);
	float inverseSquare = 0.8f * 69.9912491f * 69.9912491f *
		rcp(distanceToLight * distanceToLight + light.sizeBias);
	float t = saturate((light.radius - distanceToLight) * light.fadeZone);
	inverseSquare *= t * t * (3.0f - 2.0f * t);
	float intensityFactor = saturate(distanceToLight * light.invRadius);
	float regular = 1.0f - intensityFactor * intensityFactor;
	return lerp(regular, inverseSquare, float((light.lightFlags & (1u << 10)) != 0)) * enabled;
}

float3 CSDeferredTransformLight(float3 color, bool isLinear, float gamma, float multiplier, bool linearLighting)
{
	// Color::DirectionalLight/PointLight compensate traditional (non-linear)
	// Lambert lighting by PI. Together with VanillaNormalization this is part of
	// the existing CS visual contract, not an optional energy correction.
	return CSLightingTransformLight(color, isLinear, linearLighting, gamma, multiplier,
		linearLighting ? 1.0f : 3.14159265358979323846f);
}

float3 CSDeferredVanillaDiffuse(float3 normal, float3 lightDirection, float3 lightColor,
	float shadow, float vanillaNormalization)
{
	return CSLightingVanillaDiffuse(normal, lightDirection, lightColor, shadow, vanillaNormalization);
}

float3 CSDeferredAmbient(LightingContext context, float3 normal, bool linearLighting,
	float ambientGamma, float ambientMultiplier)
{
	float4 direction = float4(normal, 1.0f);
	float3 ambient = max(0.0f, float3(dot(context.directionalAmbient[0], direction),
		dot(context.directionalAmbient[1], direction), dot(context.directionalAmbient[2], direction)));
	return CSLightingTransformAmbient(ambient, linearLighting, ambientGamma, ambientMultiplier);
}

float3 CSDeferredIrradianceToGamma(float3 irradiance, bool linearLighting)
{
	return CSLightingIrradianceToGamma(irradiance, linearLighting);
}

#endif

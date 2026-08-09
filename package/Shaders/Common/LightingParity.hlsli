#ifndef CS_LIGHTING_PARITY_HLSLI
#define CS_LIGHTING_PARITY_HLSLI

#include "Common/Game.hlsli"

// Explicit-input lighting operations shared by Skyrim's geometry shaders and
// the D3D12 deferred path. Keep operation order stable: these are visual ABI.
float3 CSLightingTransformLight(float3 color, bool isLinear, bool linearLighting,
	float lightGamma, float multiplier, float baseCompensation)
{
	color = (linearLighting && !isLinear) ? pow(abs(color), lightGamma) : color;
	color *= baseCompensation;
	return (linearLighting && !isLinear) ?
		color * 3.14159265358979323846f * multiplier : color;
}

float3 CSLightingTransformAmbient(float3 color, bool linearLighting,
	float ambientGamma, float ambientMultiplier)
{
	return linearLighting ? pow(abs(color), ambientGamma) * ambientMultiplier : color;
}

float3 CSLightingVanillaDiffuse(float3 normal, float3 lightDirection,
	float3 lightColor, float shadow, float vanillaNormalization)
{
	return saturate(dot(normal, lightDirection)) * lightColor * shadow * vanillaNormalization;
}

float CSLightingVanillaSpecularMultiplier(float3 normal, float3 viewDirection,
	float3 lightDirection, float shininess, bool exponentialSpecular)
{
	// Inputs are normalized by the caller's lighting-context adapter. Avoid
	// normalizing them again so the forward path retains its operation order.
	float3 halfVector = normalize(viewDirection + lightDirection);
	float HdotN = saturate(dot(halfVector, normal));
	return exponentialSpecular ?
		(HdotN > 0.0f ? exp2(shininess * log2(HdotN)) : 0.0f) : HdotN;
}

struct CSGenericDirectLighting
{
	float3 diffuse;
	float3 specular;
};

CSGenericDirectLighting CSLightingEvaluateGenericDirect(float3 normal,
	float3 viewDirection, float3 lightDirection, float3 lightColor,
	float detailedShadow, float shininess, float glossiness, float3 specularColor,
	float vanillaNormalization, bool exponentialSpecular)
{
	CSGenericDirectLighting output;
	output.diffuse = CSLightingVanillaDiffuse(normal, lightDirection, lightColor,
		detailedShadow, vanillaNormalization);
	output.specular = CSLightingVanillaSpecularMultiplier(normal, viewDirection,
		lightDirection, shininess, exponentialSpecular) * specularColor * glossiness *
		lightColor * detailedShadow * vanillaNormalization;
	return output;
}

float CSLightingAttenuation(float distanceToLight, float radius, float invRadius,
	float fadeZone, float sizeBias, float inverseSquareScale, bool disabled,
	bool inverseSquareEnabled)
{
	float inverseSquare = inverseSquareScale * rcp(distanceToLight * distanceToLight + sizeBias);
	float t = saturate((radius - distanceToLight) * fadeZone);
	inverseSquare *= t * t * (3.0f - 2.0f * t);
	float intensityFactor = saturate(distanceToLight * invRadius);
	float regular = 1.0f - intensityFactor * intensityFactor;
	return lerp(regular, inverseSquare, float(inverseSquareEnabled)) * float(!disabled);
}

float3 CSLightingDirectionalAmbient(float4 ambientRow0, float4 ambientRow1,
	float4 ambientRow2, float3 normal, bool linearLighting, float ambientGamma,
	float ambientMultiplier)
{
	float4 direction = float4(normal, 1.0f);
	float3 ambient = max(0.0f, float3(dot(ambientRow0, direction),
		dot(ambientRow1, direction), dot(ambientRow2, direction)));
	return CSLightingTransformAmbient(ambient, linearLighting, ambientGamma, ambientMultiplier);
}

float3 CSLightingIrradianceToGamma(float3 color, bool linearLighting)
{
	return linearLighting ? color : pow(abs(color), 1.0f / 1.6f);
}

#endif

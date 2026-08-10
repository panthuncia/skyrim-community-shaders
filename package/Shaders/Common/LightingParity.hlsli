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

float CSLightingGrassSoftMultiplier(float angle, float rolloff)
{
	float softLight = saturate((rolloff + angle) / (1.0f + rolloff));
	float softCurve = softLight * softLight * (3.0f - 2.0f * softLight);
	float clampedAngle = saturate(angle);
	float diffuseCurve = clampedAngle * clampedAngle * (3.0f - 2.0f * clampedAngle);
	return saturate(softCurve - diffuseCurve);
}

float3 CSLightingGrassSpecular(float3 lightDirection, float3 viewDirection,
	float3 normal, float3 lightColor, float shininess)
{
	float3 halfVector = normalize(viewDirection + lightDirection);
	float HdotN = saturate(dot(halfVector, normal));
	return lightColor * exp2(shininess * log2(HdotN));
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

float CSLightingLuminance(float3 color)
{
	return dot(color, float3(0.2125f, 0.7154f, 0.0721f));
}

float3 CSLightingSaturation(float3 color, float saturation)
{
	float grey = CSLightingLuminance(color);
	return max(lerp(grey.xxx, color, saturation), 0.0f);
}

void CSLightingDiffuseIBLComponents(float3 vanillaAmbient, float3 ambientAtZero,
	float3 environment, float3 environmentAtZero, float3 sky, uint dalcMode, float dalcAmount,
	float environmentScale, float skyScale, float environmentSaturation,
	float skySaturation, bool interior, out float3 environmentResult,
	out float3 skyResult)
{
	skyResult = interior ? 0.0f : CSLightingSaturation(sky, skySaturation) * skyScale;
	if (dalcMode >= 2u) {
		environmentResult = vanillaAmbient * dalcAmount;
		return;
	}
	float3 ratio;
	if (dalcMode == 1u)
		ratio = lerp(1.0f, ambientAtZero / max(environmentAtZero, 0.001f), dalcAmount);
	else {
		float environmentLuminance = CSLightingLuminance(environmentAtZero);
		float scalarRatio = environmentLuminance > 0.001f ?
			CSLightingLuminance(ambientAtZero) / environmentLuminance : 1.0f;
		ratio = lerp(1.0f, scalarRatio.xxx, dalcAmount);
	}
	environmentResult = CSLightingSaturation(environment, environmentSaturation) *
		environmentScale * ratio;
}

void CSLightingApplySkylighting(inout float3 diffuse, inout float3 ambient,
	float3 multiBounceVisibility, bool linearLighting)
{
	float scale = 1.0f;
	if (ambient.x > 0.0f) scale = min(scale, diffuse.x / ambient.x);
	if (ambient.y > 0.0f) scale = min(scale, diffuse.y / ambient.y);
	if (ambient.z > 0.0f) scale = min(scale, diffuse.z / ambient.z);
	ambient *= scale;
	diffuse = max(0.0f, diffuse - ambient);
	float3 linearAmbient = linearLighting ? ambient : pow(abs(ambient), 1.6f);
	ambient = CSLightingIrradianceToGamma(linearAmbient * multiBounceVisibility,
		linearLighting);
	diffuse += ambient;
}

#endif

#ifndef CS_LIGHTING_PARITY_HLSLI
#define CS_LIGHTING_PARITY_HLSLI

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

float3 CSLightingIrradianceToGamma(float3 color, bool linearLighting)
{
	return linearLighting ? color : pow(abs(color), 1.0f / 1.6f);
}

#endif

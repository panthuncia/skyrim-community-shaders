#ifndef CS_DEFERRED_INDIRECT_LIGHTING_HLSLI
#define CS_DEFERRED_INDIRECT_LIGHTING_HLSLI

#include "Common/Shading.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

static const uint3 CS_SKYLIGHT_ARRAY_DIM = uint3(256, 256, 128);
static const float3 CS_SKYLIGHT_ARRAY_SIZE = 10000.0f * float3(1, 1, 0.5f);
static const float3 CS_SKYLIGHT_CELL_SIZE = CS_SKYLIGHT_ARRAY_SIZE / CS_SKYLIGHT_ARRAY_DIM;

float3 CSDeferredReadIBL(Texture2D<float4> texture, float3 direction)
{
	return float3(
		SphericalHarmonics::SHHallucinateZH3Irradiance(texture.Load(int3(0, 0, 0)), direction),
		SphericalHarmonics::SHHallucinateZH3Irradiance(texture.Load(int3(1, 0, 0)), direction),
		SphericalHarmonics::SHHallucinateZH3Irradiance(texture.Load(int3(2, 0, 0)), direction)) / 3.14159265358979323846f;
}

float3 CSDeferredDiffuseIBL(Texture2D<float4> environmentSH, Texture2D<float4> skySH,
	float3 vanillaAmbient, float3 ambientAtZero, float3 direction, uint4 flags, float4 settings0, float4 settings1,
	bool interior)
{
	if (flags.x == 0u)
		return vanillaAmbient;
	float3 environment = 0.0f;
	float3 environmentZero = 0.0f;
	if (flags.w < 2u) {
		environment = CSDeferredReadIBL(environmentSH, direction);
		environmentZero = CSDeferredReadIBL(environmentSH, 0.0f);
	}
	float3 environmentResult, skyResult;
	CSLightingDiffuseIBLComponents(vanillaAmbient, ambientAtZero, environment, environmentZero,
		CSDeferredReadIBL(skySH, direction), flags.w, settings0.x, settings0.y,
		settings0.z, settings0.w, settings1.x, interior, environmentResult, skyResult);
	return environmentResult + skyResult;
}

float CSDeferredSkylightFade(float3 positionMS)
{
	float3 uvw = saturate(positionMS / CS_SKYLIGHT_ARRAY_SIZE + 0.5f);
	float3 distances = min(uvw, 1.0f - uvw);
	return saturate(min(distances.x, min(distances.y, distances.z)) * 20.0f);
}

float4 CSDeferredSampleSkylighting(Texture2DArray<float4> probes,
	Texture2DArray<float> shadowVisibility, float3 positionMS, float3 normalWS,
	float3 positionOffset, uint3 arrayOrigin, out float directionalVisibility)
{
	const float4 unitSH = float4(sqrt(4.0f * 3.14159265358979323846f), 0, 0, 0);
	directionalVisibility = 1.0f;
	positionMS += normalWS * CS_SKYLIGHT_CELL_SIZE * 0.5f;
	float3 adjusted = positionMS - positionOffset;
	float3 uvw = adjusted / CS_SKYLIGHT_ARRAY_SIZE + 0.5f;
	if (any(uvw < 0.0f) || any(uvw > 1.0f))
		return unitSH / 1e-10f;
	float3 vertex = uvw * CS_SKYLIGHT_ARRAY_DIM;
	int3 cell000 = floor(vertex - 0.5f);
	float3 trilinearPosition = vertex - 0.5f - cell000;
	float4 sum = 0.0f; float weightSum = 0.0f;
	float shadowSum = 0.0f; float shadowWeightSum = 0.0f;
	[unroll] for (int i = 0; i < 2; ++i)
	[unroll] for (int j = 0; j < 2; ++j)
	[unroll] for (int k = 0; k < 2; ++k) {
		int3 offset = int3(i, j, k); int3 cell = cell000 + offset;
		if (any(cell < 0) || any((uint3)cell >= CS_SKYLIGHT_ARRAY_DIM)) continue;
		float3 weights = 1.0f - abs(offset - trilinearPosition);
		float trilinearWeight = weights.x * weights.y * weights.z;
		uint3 texel = ((uint3)cell + arrayOrigin) % CS_SKYLIGHT_ARRAY_DIM;
		float3 centre = (cell + 0.5f - CS_SKYLIGHT_ARRAY_DIM / 2) * CS_SKYLIGHT_CELL_SIZE;
		float tangentWeight = dot(normalize(centre - adjusted), normalWS) * 0.5f + 0.5f;
		float weight = trilinearWeight * tangentWeight;
		sum += probes.Load(int4(texel, 0)) * weight; weightSum += weight;
		shadowSum += shadowVisibility.Load(int4(texel, 0)) * trilinearWeight;
		shadowWeightSum += trilinearWeight;
	}
	directionalVisibility = lerp(1.0f, shadowSum / max(shadowWeightSum, 1e-6f), CSDeferredSkylightFade(positionMS));
	return sum / (weightSum + 1e-6f);
}

float CSDeferredSkylightDiffuse(float4 probe, float3 positionMS, float3 normal,
	float vertexAO, float minVisibility)
{
	float3 candidate = float3(normal.xy, max(0.0f, normal.z));
	float3 biased = dot(candidate, candidate) > 1e-6f ? normalize(candidate) : float3(0, 0, 1);
	float visibility = SphericalHarmonics::FuncProductIntegral(probe,
		SphericalHarmonics::EvaluateCosineLobe(biased)) / 3.14159265358979323846f;
	visibility = lerp(1.0f, saturate(visibility), CSDeferredSkylightFade(positionMS));
	return saturate(lerp(minVisibility, 1.0f, visibility) / max(vertexAO, 1e-6f));
}

void CSDeferredApplySkylighting(inout float3 diffuse, inout float3 ambient,
	float3 albedo, float visibility, bool linearLighting)
{
	CSLightingApplySkylighting(diffuse, ambient, MultiBounceAO(albedo, visibility),
		linearLighting);
}

#endif

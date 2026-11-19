#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif

// Dual-target (Common/OrgResources.hlsli). The graph reads the constants from a latch.
#include "Common/OrgResources.hlsli"
#include "Common/Random.hlsli"
#include "PhysicalSky/AlligatorNoise.hlsli"

#define NDF_NOISE_FIELDS(M, A) \
	M(uint, noiseType)         \
	M(uint, seed)              \
	M(uint, frequency)         \
	M(uint, octaves)           \
	M(float, persistence)      \
	M(uint, lacunarity)        \
	M(float, contrast)         \
	M(float, bias)             \
	M(uint, repetitions)       \
	M(float, responseExponent) \
	M(float2, padding)

#define NDF_NOISE_RESOURCES(X)            \
	X(UAV, RWTexture2D<float>, Output, 0) \
	X(LATCH, NdfNoiseConstants, CB, 1)
ORG_PUSH_BLOCK(NDF_NOISE_RESOURCES(ORG_HANDLE))

ORG_GLOBAL_LATCH_BEGIN(NdfNoiseConstants, CB, 1)
NDF_NOISE_FIELDS(ORG_GLOBAL_MEMBER, ORG_GLOBAL_ARRAY)
ORG_GLOBAL_LATCH_END(NdfNoiseConstants, CB, 1)
#define ORG_GLOBAL_LATCH CB
NDF_NOISE_FIELDS(ORG_GLOBAL_STATIC, ORG_GLOBAL_STATIC_ARRAY)
#undef ORG_GLOBAL_LATCH

ORG_RESOURCE(UAV, RWTexture2D<float>, Output, 0)

float3 CellHash(int3 cell, uint period, uint salt)
{
	cell.xy = (cell.xy % int(period) + int(period)) % int(period);
	return float3(Random::pcg3d(asuint(cell) + uint3(seed, salt, seed ^ salt)) >> 8u) / 16777216.0;
}

float Gradient(int2 cell, float2 delta, uint period)
{
	const float angle = CellHash(int3(cell, 0), period, 17u).x * 6.28318530718;
	return dot(float2(cos(angle), sin(angle)), delta);
}

float Perlin(float2 uv, uint period)
{
	const float2 p = frac(uv) * period;
	const int2 cell = int2(floor(p));
	const float2 f = frac(p);
	const float2 w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	return lerp(lerp(Gradient(cell, f, period), Gradient(cell + int2(1, 0), f - float2(1, 0), period), w.x),
		lerp(Gradient(cell + int2(0, 1), f - float2(0, 1), period), Gradient(cell + 1, f - 1.0, period), w.x), w.y);
}

float Worley(float2 uv, uint period)
{
	const float3 p = float3(frac(uv) * period, 0.5);
	const int3 cell = int3(floor(p));
	const float3 f = frac(p);
	float nearest = 2.0;
	[unroll] for (int z = -1; z <= 1; ++z)
		[unroll] for (int y = -1; y <= 1; ++y)
			[unroll] for (int x = -1; x <= 1; ++x)
	{
		const int3 offset = int3(x, y, z);
		const float distance = length(float3(offset) + CellHash(cell + offset, period, 31u) - f);
		nearest = min(nearest, distance);
	}
	return 1.0 - saturate(nearest);
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	uint2 size;
	Output.GetDimensions(size.x, size.y);
	if (any(tid >= size))
		return;
	const float2 uv = (float2(tid) + 0.5) / size * repetitions;
	float sum = 0.0;
	float weight = 1.0;
	float normalization = 0.0;
	uint period = frequency;
	[loop] for (uint octave = 0u; octave < octaves; ++octave)
	{
		if (period > min(size.x, size.y) / (2u * repetitions))
			break;
		float value;
		if (noiseType == 0u)
			value = saturate(AlligatorNoise::Sample(float3(frac(uv) * period, 0.5), uint3(period, period, 0u), seed, 31u) * 2.0);
		else {
			const float perlin = saturate(0.5 + Perlin(uv, period));
			if (noiseType == 2u) {
				const float cellular = Worley(uv, period);
				value = lerp(cellular, 1.0, perlin);
			} else
				value = perlin;
		}
		sum += value * weight;
		normalization += weight;
		period *= lacunarity;
		weight *= persistence;
	}
	const float value = (sum / max(normalization, 1e-5) - 0.5) * contrast + 0.5;
	Output[tid] = saturate((responseExponent == 1.0 ? value : pow(saturate(value), responseExponent)) + bias);
}

#define COMPUTESHADER
// Dual-target (Common/OrgResources.hlsli). The graph reads the parameters from a latch. All entries share one list.
#include "Common/OrgResources.hlsli"
#include "Common/Random.hlsli"
#include "PhysicalSky/AlligatorNoise.hlsli"

struct NoiseBand
{
	uint frequency;
	uint octaves;
	float persistence;
	float exponent;
	float contrast;
	float bias;
	float perlinMix;
	float padding;
};

#define CLOUD_NOISE_FIELDS(M, A) \
	A(NoiseBand, shape, 4)       \
	A(NoiseBand, warp, 2)        \
	M(uint, seed)                \
	M(uint, firstSlice)          \
	M(uint, mip)                 \
	M(float, warpCorrelation)

#define CLOUD_NOISE_RESOURCES(X)                           \
	X(SRV, Texture2D<float4>, SourceAdjustment, 0)         \
	X(SRV, Texture2D<float4>, PreviousAdjustment, 1)       \
	X(SRV, Texture3D<float4>, PreviousShape, 2)            \
	X(UAV, RWTexture3D<unorm float4>, ShapeOutput, 0)      \
	X(UAV, RWTexture2D<unorm float4>, AdjustmentOutput, 1) \
	X(LATCH, CloudNoiseConstants, Parameters, 1)
ORG_PUSH_BLOCK(CLOUD_NOISE_RESOURCES(ORG_HANDLE))

ORG_GLOBAL_LATCH_BEGIN(CloudNoiseConstants, Parameters, 1)
CLOUD_NOISE_FIELDS(ORG_GLOBAL_MEMBER, ORG_GLOBAL_ARRAY)
ORG_GLOBAL_LATCH_END(CloudNoiseConstants, Parameters, 1)
#define ORG_GLOBAL_LATCH Parameters
CLOUD_NOISE_FIELDS(ORG_GLOBAL_STATIC, ORG_GLOBAL_STATIC_ARRAY)
#undef ORG_GLOBAL_LATCH

ORG_RESOURCE(SRV, Texture2D<float4>, SourceAdjustment, 0)
ORG_RESOURCE(SRV, Texture2D<float4>, PreviousAdjustment, 1)
ORG_RESOURCE(SRV, Texture3D<float4>, PreviousShape, 2)
ORG_RESOURCE(UAV, RWTexture3D<unorm float4>, ShapeOutput, 0)
ORG_RESOURCE(UAV, RWTexture2D<unorm float4>, AdjustmentOutput, 1)

float3 CellHash(int3 cell, uint period, uint salt)
{
	cell = (cell % int(period) + int(period)) % int(period);
	return float3(Random::pcg3d(asuint(cell) + uint3(seed, salt, seed ^ salt)) >> 8u) / 16777216.0;
}

float Perlin(float3 uv, uint period, uint salt)
{
	const float3 p = frac(uv) * period;
	const int3 cell = int3(floor(p));
	const float3 f = frac(p);
	const float3 fade = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float result = 0.0;
	[unroll] for (uint z = 0u; z < 2u; ++z)
		[unroll] for (uint y = 0u; y < 2u; ++y)
			[unroll] for (uint x = 0u; x < 2u; ++x)
	{
		const int3 corner = int3(x, y, z);
		float3 gradient = CellHash(cell + corner, period, salt) * 2.0 - 1.0;
		gradient *= rsqrt(max(dot(gradient, gradient), 1e-16));
		const float3 weight = lerp(1.0 - fade, fade, float3(corner));
		result += dot(gradient, f - corner) * weight.x * weight.y * weight.z;
	}
	return saturate(0.5 + result);
}

float Worley(float3 uv, uint period, uint salt)
{
	const float3 p = frac(uv) * period;
	const int3 cell = int3(floor(p));
	const float3 f = frac(p);
	float nearestSquared = 1.0;
	[loop] for (int z = -1; z <= 1; ++z)
		[loop] for (int y = -1; y <= 1; ++y)
			[loop] for (int x = -1; x <= 1; ++x)
	{
		const int3 offset = int3(x, y, z);
		const float3 delta = float3(offset) + CellHash(cell + offset, period, salt) - f;
		const float distanceSquared = dot(delta, delta);
		nearestSquared = min(nearestSquared, distanceSquared);
	}
	return 1.0 - sqrt(nearestSquared);
}

float Fractal(float3 uv, NoiseBand band, uint salt, uint mode, uint maxPeriod)
{
	float sum = 0.0;
	float weight = 1.0;
	float normalization = 0.0;
	uint period = band.frequency;
	[loop] for (uint octave = 0u; octave < band.octaves && period <= maxPeriod; ++octave)
	{
		const uint octaveSalt = salt + octave * 101u;
		float value;
		if (mode == 2u)
			value = Perlin(uv, period, octaveSalt);
		else if (mode == 1u)
			value = AlligatorNoise::Sample(frac(uv) * period, uint3(period, period, period), seed, octaveSalt);
		else {
			value = Worley(uv, period, octaveSalt);
			value = lerp(value, 1.0, Perlin(uv, period, octaveSalt + 47u) * band.perlinMix);
		}
		sum += value * weight;
		normalization += weight;
		weight *= band.persistence;
		period *= 2u;
	}
	return sum / max(normalization, 1e-8);
}

float Response(float value, NoiseBand band)
{
	return saturate((pow(saturate(value), band.exponent) - 0.5) * band.contrast + 0.5 + band.bias);
}

[numthreads(4, 4, 4)] void generateShape(uint3 tid : SV_DispatchThreadID) {
	uint3 size;
	ShapeOutput.GetDimensions(size.x, size.y, size.z);
	tid.z += firstSlice;
	if (any(tid >= size))
		return;
	const float3 uv = (float3(tid) + 0.5) / size;
	float4 result;
	[unroll] for (uint channel = 0u; channel < 4u; ++channel)
		result[channel] = Response(Fractal(uv, shape[channel], 17u + channel * 1019u,
									   channel == 0u ? 0u : 1u, min(min(size.x, size.y), size.z) / 2u),
			shape[channel]);
	ShapeOutput[tid] = result;
}

	[numthreads(8, 8, 1)] void generateAdjustment(uint2 tid : SV_DispatchThreadID)
{
	uint2 size;
	AdjustmentOutput.GetDimensions(size.x, size.y);
	if (any(tid >= size))
		return;
	const float3 uv = float3((float2(tid) + 0.5) / size, 0.371);
	const uint maxPeriod = max(min(size.x, size.y) / 2u, 1u);
	const float first = Fractal(uv, warp[0], 701u, 2u, maxPeriod);
	const float independent = Fractal(uv, warp[1], 1709u, 2u, maxPeriod);
	const float second = saturate(0.5 + warpCorrelation * (first - 0.5) + sqrt(1.0 - warpCorrelation * warpCorrelation) * (independent - 0.5));
	AdjustmentOutput[tid] = float4(SourceAdjustment.Load(int3(tid, 0)).r,
		Response(first, warp[0]), Response(second, warp[1]), 1.0);
}

[numthreads(8, 8, 1)] void generateAdjustmentMip(uint2 tid : SV_DispatchThreadID) {
	uint2 size;
	AdjustmentOutput.GetDimensions(size.x, size.y);
	if (any(tid >= size))
		return;
	float2 displacement = 0.0;
	uint2 previousSize;
	PreviousAdjustment.GetDimensions(previousSize.x, previousSize.y);
	[unroll] for (uint y = 0u; y < 2u; ++y)
		[unroll] for (uint x = 0u; x < 2u; ++x)
			displacement += PreviousAdjustment.Load(int3(min(tid * 2u + uint2(x, y), previousSize - 1u), 0)).gb;
	AdjustmentOutput[tid] = float4(SourceAdjustment.Load(int3(tid, mip)).r, displacement * 0.25, 1.0);
}

// One mip of the 3D shape noise, a box filter like D3D11's GenerateMips. The graph has no 3D GenerateMips.
// PreviousShape is the mip above, ShapeOutput this mip. Each texel averages a 2x2x2 block.
// Reads clamp to the previous mip's size, so odd sizes work.
[numthreads(4, 4, 4)] void generateShapeMip(uint3 tid : SV_DispatchThreadID) {
	uint3 size;
	ShapeOutput.GetDimensions(size.x, size.y, size.z);
	if (any(tid >= size))
		return;
	uint3 previousSize;
	PreviousShape.GetDimensions(previousSize.x, previousSize.y, previousSize.z);
	float4 sum = 0.0;
	[unroll] for (uint z = 0u; z < 2u; ++z)
		[unroll] for (uint y = 0u; y < 2u; ++y)
			[unroll] for (uint x = 0u; x < 2u; ++x)
				sum += PreviousShape.Load(int4(min(tid * 2u + uint3(x, y, z), previousSize - 1u), 0));
	ShapeOutput[tid] = sum * 0.125;
}

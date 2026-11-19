#define COMPUTESHADER

// Dual-target (Common/OrgResources.hlsli). The graph reads the parameters from a latch.
#include "Common/OrgResources.hlsli"

struct NoiseLayer
{
	uint noise;
	float frequency;
	float2 offset;
	float4 range;
};

// weather: primary, type, secondary, coverage gain, thickness variation.
// shape: coverage-to-height exponent, displacement / nominal thickness.
#define MID_CLOUD_FIELDS(M, A) \
	A(NoiseLayer, weather, 5)  \
	M(float, localWeight)      \
	M(uint, localBlendMode)    \
	M(float, weight)           \
	M(uint, hasPrevious)       \
	M(float4, shape)

#define MID_CLOUD_RESOURCES(X)                    \
	X(SRV, Texture2D<float>, Noise0, 0)           \
	X(SRV, Texture2D<float>, Noise1, 1)           \
	X(SRV, Texture2D<float>, Noise2, 2)           \
	X(SRV, Texture2D<float>, Noise3, 3)           \
	X(SRV, Texture2D<float4>, LocalWeather, 4)    \
	X(SRV, Texture2D<float4>, PreviousWeather, 5) \
	X(UAV, RWTexture2D<float4>, OutputWeather, 0) \
	X(SAMPLER, SamplerState, NoiseSampler, 0)     \
	X(LATCH, MidCloudConstants, Parameters, 1)
ORG_PUSH_BLOCK(MID_CLOUD_RESOURCES(ORG_HANDLE))

ORG_GLOBAL_LATCH_BEGIN(MidCloudConstants, Parameters, 1)
MID_CLOUD_FIELDS(ORG_GLOBAL_MEMBER, ORG_GLOBAL_ARRAY)
ORG_GLOBAL_LATCH_END(MidCloudConstants, Parameters, 1)
#define ORG_GLOBAL_LATCH Parameters
MID_CLOUD_FIELDS(ORG_GLOBAL_STATIC, ORG_GLOBAL_STATIC_ARRAY)
#undef ORG_GLOBAL_LATCH

ORG_RESOURCE(SRV, Texture2D<float>, Noise0, 0)
ORG_RESOURCE(SRV, Texture2D<float>, Noise1, 1)
ORG_RESOURCE(SRV, Texture2D<float>, Noise2, 2)
ORG_RESOURCE(SRV, Texture2D<float>, Noise3, 3)
ORG_RESOURCE(SRV, Texture2D<float4>, LocalWeather, 4)
ORG_RESOURCE(SRV, Texture2D<float4>, PreviousWeather, 5)
ORG_RESOURCE(SAMPLER, SamplerState, NoiseSampler, 0)
ORG_RESOURCE(UAV, RWTexture2D<float4>, OutputWeather, 0)

float SampleNoise(Texture2D<float> source, float2 uv, float frequency, uint2 size)
{
	uint2 dimensions;
	source.GetDimensions(dimensions.x, dimensions.y);
	const float2 footprint = float2(dimensions) * abs(frequency) / size;
	return source.SampleLevel(NoiseSampler, uv, log2(max(max(footprint.x, footprint.y), 1.0)));
}

float WeatherValue(NoiseLayer layer, float2 uv, uint2 size)
{
	// Neutral extension fields and constant cloud types need no texture query.
	if (layer.range.z == layer.range.w)
		return layer.range.z;
	float n = 0.0;
	const float2 p = (uv + layer.offset) * layer.frequency;
	switch (layer.noise) {
	case 0u:
		n = SampleNoise(Noise0, p, layer.frequency, size);
		break;
	case 1u:
		n = SampleNoise(Noise1, p, layer.frequency, size);
		break;
	case 2u:
		n = SampleNoise(Noise2, p, layer.frequency, size);
		break;
	case 3u:
		n = SampleNoise(Noise3, p, layer.frequency, size);
		break;
	}
	return lerp(layer.range.z, layer.range.w, saturate((n * 2.0 - 1.0 - layer.range.x) / (layer.range.y - layer.range.x)));
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	uint2 size;
	OutputWeather.GetDimensions(size.x, size.y);
	if (any(tid >= size))
		return;
	const float2 uv = (float2(tid) + 0.5) / size;
	float2 controls = float2(WeatherValue(weather[0], uv, size), saturate(WeatherValue(weather[1], uv, size)));
	controls.x = WeatherValue(weather[3], uv, size) * max(controls.x, WeatherValue(weather[2], uv, size));
	if (localWeight > 0.0) {
		uint2 dimensions;
		LocalWeather.GetDimensions(dimensions.x, dimensions.y);
		const float mip = log2(max(max(float(dimensions.x) / size.x, float(dimensions.y) / size.y), 1.0));
		const float3 local = saturate(LocalWeather.SampleLevel(NoiseSampler, uv, mip).rgb);
		controls = localBlendMode == 0u ? lerp(controls, local.rg, local.b * localWeight) : max(controls, local.rg * localWeight);
	}
	const float coverHeight = saturate(pow(saturate(controls.x), shape.x) + WeatherValue(weather[4], uv, size));
	float bottom = 0.2 * (1.0 - coverHeight);
	float top = 0.2 + 0.8 * coverHeight;
	// A collapsed layer has no material and must not pull the geometry of a
	// nonempty scene endpoint toward its zero-thickness bounds during blending.
	float density = coverHeight > 0.0 ? smoothstep(0.15, 0.65, controls.x) : 0.0;
	// Only encode the nominal layer inside its conservative displaced support.
	// The band phase is evaluated in world space, independently of this tile.
	bottom = (bottom + shape.y) / (1.0 + 2.0 * shape.y);
	top = (top + shape.y) / (1.0 + 2.0 * shape.y);
	// Density-weighted geometry keeps fading empty weather from dragging the
	// occupied layer to height zero. Decode after spatial/scene interpolation.
	float4 result = float4(1.0, bottom, top, controls.y) * (density * weight);
	if (hasPrevious != 0u)
		result += PreviousWeather.Load(int3(tid, 0));
	OutputWeather[tid] = result;
}

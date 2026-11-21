#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif

// Dual-target (Common/OrgResources.hlsli). The graph reads the constants from a latch.
#include "Common/OrgResources.hlsli"

struct NoiseLayer
{
	uint noise;
	float frequency;
	float exponent;
	float padding0;
	float2 offset;
	float2 padding1;
	float4 range;
};

#define NDF_GENERATE_FIELDS(M, A)   \
	M(NoiseLayer, primary)          \
	M(NoiseLayer, secondary)        \
	M(NoiseLayer, coverageGain)     \
	M(NoiseLayer, modeling)         \
	M(NoiseLayer, modelingGain)     \
	M(NoiseLayer, heightVariation)  \
	M(float4, heightAuxiliaryRange) \
	M(float4, bottomTypeRange)      \
	M(float2, baseHeight)           \
	M(float, bottomTypeExponent)    \
	M(uint, heightFromCoverage)     \
	M(uint, localBlendMode)         \
	M(float, localModelingWeight)   \
	M(float, localHeightWeight)     \
	M(float, localWindScale)        \
	M(float2, windOffset)           \
	M(uint, hasLocalMask)           \
	M(uint, imported)               \
	M(uint, preview)                \
	M(float3, padding)

#define NDF_GENERATE_RESOURCES(X)                  \
	X(SRV, Texture2D<float4>, Noise0, 0)           \
	X(SRV, Texture2D<float4>, Noise1, 1)           \
	X(SRV, Texture2D<float4>, Noise2, 2)           \
	X(SRV, Texture2D<float4>, Noise3, 3)           \
	X(SRV, Texture2D<float4>, LocalModeling, 4)    \
	X(SRV, Texture2D<float2>, LocalHeight, 5)      \
	X(SRV, Texture2D<float>, LocalMask, 6)         \
	X(UAV, RWTexture2D<float4>, OutputHeight, 0)   \
	X(UAV, RWTexture2D<float4>, OutputModeling, 1) \
	X(UAV, RWTexture2D<float4>, OutputPreview, 2)  \
	X(SAMPLER, SamplerState, NoiseSampler, 0)      \
	X(LATCH, NdfGenerateConstants, CB, 1)
ORG_PUSH_BLOCK(NDF_GENERATE_RESOURCES(ORG_HANDLE))

ORG_GLOBAL_LATCH_BEGIN(NdfGenerateConstants, CB, 1)
NDF_GENERATE_FIELDS(ORG_GLOBAL_MEMBER, ORG_GLOBAL_ARRAY)
ORG_GLOBAL_LATCH_END(NdfGenerateConstants, CB, 1)
#define ORG_GLOBAL_LATCH CB
NDF_GENERATE_FIELDS(ORG_GLOBAL_STATIC, ORG_GLOBAL_STATIC_ARRAY)
#undef ORG_GLOBAL_LATCH

ORG_RESOURCE(SRV, Texture2D<float4>, Noise0, 0)
ORG_RESOURCE(SRV, Texture2D<float4>, Noise1, 1)
ORG_RESOURCE(SRV, Texture2D<float4>, Noise2, 2)
ORG_RESOURCE(SRV, Texture2D<float4>, Noise3, 3)
ORG_RESOURCE(SRV, Texture2D<float4>, LocalModeling, 4)
ORG_RESOURCE(SRV, Texture2D<float2>, LocalHeight, 5)
ORG_RESOURCE(SRV, Texture2D<float>, LocalMask, 6)
ORG_RESOURCE(SAMPLER, SamplerState, NoiseSampler, 0)
ORG_RESOURCE(UAV, RWTexture2D<float4>, OutputHeight, 0)
ORG_RESOURCE(UAV, RWTexture2D<float4>, OutputModeling, 1)
ORG_RESOURCE(UAV, RWTexture2D<float4>, OutputPreview, 2)

float NoiseMip(Texture2D<float4> source, float frequency, uint2 outputSize)
{
	uint2 size;
	source.GetDimensions(size.x, size.y);
	const float2 footprint = float2(size) * abs(frequency) / outputSize;
	return log2(max(max(footprint.x, footprint.y), 1.0));
}

float SampleNoise(uint index, float2 uv, float frequency, uint2 outputSize)
{
	switch (index) {
	case 0u:
		return Noise0.SampleLevel(NoiseSampler, uv, NoiseMip(Noise0, frequency, outputSize)).r;
	case 1u:
		return Noise1.SampleLevel(NoiseSampler, uv, NoiseMip(Noise1, frequency, outputSize)).r;
	case 2u:
		return Noise2.SampleLevel(NoiseSampler, uv, NoiseMip(Noise2, frequency, outputSize)).r;
	case 3u:
		return Noise3.SampleLevel(NoiseSampler, uv, NoiseMip(Noise3, frequency, outputSize)).r;
	default:
		return 0.0;
	}
}

float Remap(float value, float4 range)
{
	return saturate((value - range.x) / (range.y - range.x)) * (range.w - range.z) + range.z;
}

float NoisePower(float value, float exponent)
{
	// Avoid invalid logarithms when signed noise drives height.
	return value > 0.0 ? exp2(log2(value) * exponent) : 0.0;
}

float SignedNoise(NoiseLayer layer, float2 uv, float2 wind, uint2 size)
{
	return SampleNoise(layer.noise, ((layer.offset - wind) + uv) * layer.frequency, layer.frequency, size) * 2.0 - 1.0;
}

float RangedNoise(NoiseLayer layer, float2 uv, float2 wind, uint2 size)
{
	return layer.range.z == layer.range.w ? layer.range.w : Remap(SignedNoise(layer, uv, wind, size), layer.range);
}

void StoreMaps(uint2 tid, float2 height, float3 model, float first, float second, float gain, float typeGain, float variation)
{
	const float auxiliary = saturate(Remap(model.g, heightAuxiliaryRange));
	OutputHeight[tid] = float4(saturate(height), auxiliary, 0.0);
	OutputModeling[tid] = float4(saturate(model), 0.0);
	if (preview != 0u) {
		float value = 0.0;
		switch (preview) {
		case 1u:
			value = first;
			break;
		case 2u:
			value = second;
			break;
		case 3u:
			value = gain;
			break;
		case 4u:
			value = typeGain;
			break;
		case 5u:
			value = model.r;
			break;
		case 6u:
			value = model.g;
			break;
		case 7u:
			value = model.b;
			break;
		case 8u:
			value = height.x;
			break;
		case 9u:
			value = height.y;
			break;
		case 10u:
			value = auxiliary;
			break;
		case 11u:
			value = variation;
			break;
		}
		OutputPreview[tid] = float4(value.xxx, 1.0);
	}
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	uint2 size;
	OutputHeight.GetDimensions(size.x, size.y);
	if (any(tid >= size))
		return;
	const float2 uv = (float2(tid) + 0.5) / size;
	if (imported != 0u) {
		const float2 height = LocalHeight.SampleLevel(NoiseSampler, uv, 0);
		const float3 model = LocalModeling.SampleLevel(NoiseSampler, uv, NoiseMip(LocalModeling, 1.0, size)).rgb;
		StoreMaps(tid, height, model, 0.0, 0.0, 1.0, 1.0, 0.0);
		return;
	}
	const float2 wind = windOffset * 0.00005;
	float first;
	float second;
	if (primary.range.z == primary.range.w) {
		first = primary.range.w;
		second = primary.range.w;
	} else {
		first = NoisePower(Remap(SignedNoise(primary, uv, wind, size), primary.range), primary.exponent);
		second = NoisePower(Remap(SignedNoise(secondary, uv, wind, size), secondary.range), secondary.exponent);
	}
	const float gain = RangedNoise(coverageGain, uv, wind, size);
	const float typeNoise = SignedNoise(modeling, uv, wind, size);
	const float typeGain = RangedNoise(modelingGain, uv, wind, size);
	float3 model = float3(gain * max(first, second),
		typeGain * NoisePower(Remap(typeNoise, modeling.range), modeling.exponent),
		typeGain * NoisePower(Remap(typeNoise, bottomTypeRange), bottomTypeExponent));

	const float2 localUv = uv - wind * localWindScale;
	float mask = 1.0;
	if (hasLocalMask != 0u) {
		uint2 maskSize;
		LocalMask.GetDimensions(maskSize.x, maskSize.y);
		const float2 footprint = float2(maskSize) / size;
		mask = LocalMask.SampleLevel(NoiseSampler, localUv, log2(max(max(footprint.x, footprint.y), 1.0)));
	}
	if (localModelingWeight > 0.0) {
		const float3 localModel = LocalModeling.SampleLevel(NoiseSampler, localUv, NoiseMip(LocalModeling, 1.0, size)).rgb;
		if (localBlendMode == 0u)
			model = (mask * localModelingWeight) * (localModel - model) + model;
		else
			model = max(model, localModel * localModelingWeight);
	}
	float2 height = baseHeight;
	if (localHeightWeight > 0.0) {
		uint2 heightSize;
		LocalHeight.GetDimensions(heightSize.x, heightSize.y);
		const float2 footprint = float2(heightSize) / size;
		const float2 localHeight = LocalHeight.SampleLevel(NoiseSampler, localUv, log2(max(max(footprint.x, footprint.y), 1.0)));
		height = (saturate(mask) * localHeightWeight) * (localHeight - baseHeight) + baseHeight;
	}
	float variation;
	if (heightVariation.range.z == heightVariation.range.w)
		variation = heightVariation.range.z;
	else {
		const float driver = heightFromCoverage != 0u ? model.r : SignedNoise(heightVariation, uv, wind, size);
		variation = Remap(NoisePower(driver, heightVariation.exponent), heightVariation.range);
	}
	height.x += variation;
	StoreMaps(tid, height, model, first, second, gain, typeGain, variation);
}

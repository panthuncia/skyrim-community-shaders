/// Local Exposure luminance analysis
/// Builds the luminance pyramid and edge-aware grid. Composite resolves the
/// base layer and evaluates tonal remapping using the final global exposure.

#include "Common/OrgResources.hlsli"

#include "Common/Color.hlsli"

#define GRID_DEPTH 32
#define GRID_TILE_SIZE 64
#define GRID_THREAD_SIZE 8
#define GRID_SAMPLE_STRIDE 8
#define GRID_QUANTIZATION 4096
#define MAX_BLUR_RADIUS 64

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below (LocalExposure::LocalExposureCB at b1). The
// ORG chain (ORG/ORGLocalExposure.cpp) gives heap handles in the push block and the constants from its latch. Each entry
// point reads only some of the resources: the others' handles are never dereferenced.
struct LocalExposureConstants
{
	float ManualExposure;
	float Strength;
	float HighlightContrast;
	float ShadowContrast;

	float DetailStrength;
	float BaseBlend;
	float BlurRadius;
	float MiddleGreyBias;

	float HighlightThreshold;
	float ShadowThreshold;
	float HighlightThresholdStrength;
	float ShadowThresholdStrength;

	uint InputWidth;
	uint InputHeight;
	uint BlurredWidth;
	uint BlurredHeight;

	float LogLuminanceMin;
	float LogLuminanceMax;
	float2 Padding1;
};

#define LOCAL_EXPOSURE_RESOURCES(X)                    \
	X(SRV, Texture2D<float4>, TexColor, 0)             \
	X(SRV, Texture2D<float>, TexLogLuminance, 1)       \
	X(SRV, Texture3D<float2>, TexLuminanceGrid, 2)     \
	X(SRV, Texture2D<float>, TexBlurredLuminance, 3)   \
	X(SAMPLER, SamplerState, LinearSampler, 0)         \
	X(SAMPLER, SamplerState, MirrorSampler, 1)         \
	X(UAV, RWTexture2D<float>, RWTexOutput, 0)         \
	X(UAV, RWTexture3D<float2>, RWTexLuminanceGrid, 1) \
	X(LATCH, LocalExposureConstants, LocalExposure, 1)
ORG_PUSH_BLOCK(LOCAL_EXPOSURE_RESOURCES(ORG_HANDLE))
LOCAL_EXPOSURE_RESOURCES(ORG_RESOURCE)

float SceneLogLuminance(float3 color)
{
	float luminance = Color::RGBToLuminance(max(color, 0.0));
	return clamp(log2(max(luminance, exp2(LocalExposure.LogLuminanceMin))), LocalExposure.LogLuminanceMin, LocalExposure.LogLuminanceMax);
}

[numthreads(8, 8, 1)] void CSSetupLogLuminance(uint2 tid : SV_DispatchThreadID) {
	if (tid.x >= LocalExposure.InputWidth || tid.y >= LocalExposure.InputHeight)
		return;

	RWTexOutput[tid] = SceneLogLuminance(TexColor[tid].rgb);
}

	[numthreads(8, 8, 1)] void CSDownsampleLogLuminance(uint2 tid : SV_DispatchThreadID)
{
	uint2 outputSize;
	RWTexOutput.GetDimensions(outputSize.x, outputSize.y);
	if (any(tid >= outputSize))
		return;

	uint2 inputSize;
	TexLogLuminance.GetDimensions(inputSize.x, inputSize.y);

	float2 uv = (float2(tid) + 0.5) / float2(outputSize);
	float2 radius = 0.5 / float2(inputSize);
	float result = 0.0;
	result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(-radius.x, -radius.y), 0);
	result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(radius.x, -radius.y), 0);
	result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(-radius.x, radius.y), 0);
	result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(radius.x, radius.y), 0);
	RWTexOutput[tid] = result * 0.25;
}

float BlurLogLuminance(uint2 tid, float2 direction)
{
	uint2 outputSize;
	RWTexOutput.GetDimensions(outputSize.x, outputSize.y);
	float2 uv = (float2(tid) + 0.5) / float2(outputSize);
	float2 texelSize = rcp(float2(outputSize));
	float sigma = max(LocalExposure.BlurRadius * 0.173, 0.5);
	float result = 0.0;
	float weightSum = 0.0;
	int kernelRadius = min((int)ceil(LocalExposure.BlurRadius), MAX_BLUR_RADIUS);

	[loop] for (int offset = -kernelRadius; offset <= kernelRadius; offset++)
	{
		float weight = exp2(-0.72134752 * offset * offset / (sigma * sigma));
		result += TexLogLuminance.SampleLevel(MirrorSampler, uv + direction * texelSize * offset, 0) * weight;
		weightSum += weight;
	}

	return result / max(weightSum, 1e-5);
}

[numthreads(8, 8, 1)] void CSBlurHorizontal(uint2 tid : SV_DispatchThreadID) {
	if (tid.x >= LocalExposure.BlurredWidth || tid.y >= LocalExposure.BlurredHeight)
		return;

	RWTexOutput[tid] = BlurLogLuminance(tid, float2(1.0, 0.0));
}

	[numthreads(8, 8, 1)] void CSBlurVertical(uint2 tid : SV_DispatchThreadID)
{
	if (tid.x >= LocalExposure.BlurredWidth || tid.y >= LocalExposure.BlurredHeight)
		return;

	RWTexOutput[tid] = BlurLogLuminance(tid, float2(0.0, 1.0));
}

// Adjacent lanes access adjacent bins during reduction; the extra column also
// avoids a power-of-two stride when each lane updates its own histogram.
groupshared uint ThreadGridWeights[GRID_THREAD_SIZE * GRID_THREAD_SIZE][GRID_DEPTH + 1];
groupshared uint ThreadGridLogSums[GRID_THREAD_SIZE * GRID_THREAD_SIZE][GRID_DEPTH + 1];

[numthreads(GRID_THREAD_SIZE, GRID_THREAD_SIZE, 1)] void CSBuildLuminanceGrid(
	uint3 groupID : SV_GroupID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex) {
	[unroll] for (uint bin = 0; bin < GRID_DEPTH; bin++)
	{
		ThreadGridWeights[groupIndex][bin] = 0;
		ThreadGridLogSums[groupIndex][bin] = 0;
	}

	const float inverseLogRange = rcp(LocalExposure.LogLuminanceMax - LocalExposure.LogLuminanceMin);
	const uint2 tileOrigin = groupID.xy * GRID_TILE_SIZE;

	[unroll] for (uint y = 0; y < GRID_SAMPLE_STRIDE; y++)
	{
		[unroll] for (uint x = 0; x < GRID_SAMPLE_STRIDE; x++)
		{
			uint2 pixel = tileOrigin + groupThreadID.xy + uint2(x, y) * GRID_THREAD_SIZE;
			if (pixel.x < LocalExposure.InputWidth && pixel.y < LocalExposure.InputHeight) {
				float normalizedLog = saturate((TexLogLuminance[pixel] - LocalExposure.LogLuminanceMin) * inverseLogRange);
				float binPosition = normalizedLog * (GRID_DEPTH - 1);
				uint lowerBin = min((uint)binPosition, GRID_DEPTH - 1);
				uint upperBin = min(lowerBin + 1, GRID_DEPTH - 1);
				uint upperWeight = (uint)(frac(binPosition) * GRID_QUANTIZATION + 0.5);
				uint lowerWeight = GRID_QUANTIZATION - upperWeight;

				ThreadGridWeights[groupIndex][lowerBin] += lowerWeight;
				ThreadGridLogSums[groupIndex][lowerBin] += (uint)(normalizedLog * lowerWeight + 0.5);
				ThreadGridWeights[groupIndex][upperBin] += upperWeight;
				ThreadGridLogSums[groupIndex][upperBin] += (uint)(normalizedLog * upperWeight + 0.5);
			}
		}
	}

	GroupMemoryBarrierWithGroupSync();
	if (groupIndex < GRID_DEPTH) {
		uint gridWeight = 0;
		uint gridLogSum = 0;
		[unroll] for (uint threadIndex = 0; threadIndex < GRID_THREAD_SIZE * GRID_THREAD_SIZE; threadIndex++)
		{
			gridWeight += ThreadGridWeights[threadIndex][groupIndex];
			gridLogSum += ThreadGridLogSums[threadIndex][groupIndex];
		}

		const float decodeScale = rcp((float)GRID_QUANTIZATION);
		RWTexLuminanceGrid[uint3(groupID.xy, groupIndex)] = float2(gridLogSum, gridWeight) * decodeScale;
	}
}

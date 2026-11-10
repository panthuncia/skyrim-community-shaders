/**
 * Motion Blur - Blur Pass (Pass 3 of 3)
 *
 * Final pass that applies motion blur using neighborhood velocities
 * Uses depth-aware sampling to prevent foreground/background bleeding
 */

#include "Common/FrameBuffer.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/OrgResources.hlsli"
#include "PostProcessing/common.hlsli"

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below (MotionBlur::MotionBlurConstantBuffer at b0).
// The ORG chain (ORG/ORGMotionBlur.cpp) gives heap handles in the push block and the constants from its latch.
struct BlurConstants
{
	float4 VelocityParams;  // x: velocity multiplier, yz: motion-vector source/display scale
	int SampleCount;        // Number of samples
};

// TexColor: the colour. TexVelocity: the motion vectors. TexNeighborMax: the neighbourhood velocities (20 x 20 grid).
// TexDepth: the depth. RWTexOut: the output.
#define BLUR_RESOURCES(X)                       \
	X(SRV, Texture2D<float4>, TexColor, 0)       \
	X(SRV, Texture2D<float4>, TexVelocity, 1)    \
	X(SRV, Texture2D<float4>, TexNeighborMax, 2) \
	X(SRV, Texture2D<float>, TexDepth, 3)        \
	X(UAV, RWTexture2D<float4>, RWTexOut, 0)     \
	X(SAMPLER, SamplerState, LinearSampler, 0)   \
	X(SAMPLER, SamplerState, PointSampler, 1)    \
	X(LATCH, BlurConstants, Blur, 0)
ORG_PUSH_BLOCK(BLUR_RESOURCES(ORG_HANDLE))
BLUR_RESOURCES(ORG_RESOURCE)

// Fixed parameters
static const uint GRID_SIZE = 20;  // Fixed grid size
static const float g_MaxBlurRadius = 40.0f;
#define PI 3.14159265359f
#define MB_SOFTZ_INCHES 1.0f

// Extract velocity from encoded color
float2 ExtractVelocity(float4 colorSample)
{
	// Direction from R,G channels
	float2 dir;
	dir.x = (colorSample.r * 2.0f) - 1.0f;
	dir.y = (colorSample.g * 2.0f) - 1.0f;

	float dirLength = length(dir);
	if (dirLength > 0.001f)
		dir /= dirLength;

	// Magnitude from B channel
	float magnitude = colorSample.b * 20.0f;

	return dir * magnitude;
}

// Depth comparison - returns (foreground, background) weights
float2 DepthCmp(float centerDepth, float sampleDepth, float depthScale)
{
	return saturate(0.5f + float2(depthScale, -depthScale) * (sampleDepth - centerDepth));
}

// Spread comparison
float2 SpreadCmp(float offsetLen, float2 spreadLen, float pixelToSampleUnitsScale)
{
	return saturate(pixelToSampleUnitsScale * spreadLen - max(offsetLen - 1.0f, 0.0f));
}

// Calculate sample weight
float SampleWeight(
	float centerDepth, float sampleDepth, float offsetLen, float centerSpreadLen,
	float sampleSpreadLen, float pixelToSampleUnitsScale, float depthScale)
{
	float2 depthCmp = DepthCmp(centerDepth, sampleDepth, depthScale);
	float2 spreadCmp = SpreadCmp(offsetLen, float2(centerSpreadLen, sampleSpreadLen), pixelToSampleUnitsScale);
	return dot(depthCmp, spreadCmp);
}

// Generate dithered offset
float GetDitheredOffset(uint2 position, float sampleIndex)
{
	float scale = 0.25f;
	uint2 positionMod = position & 1;  // 2x2 checkerboard
	return (-scale + 2.0f * scale * positionMod.x) * (-1.0f + 2.0f * positionMod.y);
}

float2 GetVelocityTexCoord(float2 targetTexCoord)
{
	uint2 targetDimensions;
	TexColor.GetDimensions(targetDimensions.x, targetDimensions.y);

	uint2 velocityDimensions;
	TexVelocity.GetDimensions(velocityDimensions.x, velocityDimensions.y);

	float2 targetToVelocityScale = Blur.VelocityParams.yz;
	float2 sourceDimensions = max(float2(1.0f, 1.0f), float2(targetDimensions) * targetToVelocityScale);
	sourceDimensions = min(sourceDimensions, float2(velocityDimensions));

	float2 uvScale = sourceDimensions / float2(velocityDimensions);
	float2 minUV = 0.5f / float2(velocityDimensions);
	float2 maxUV = (sourceDimensions - 0.5f) / float2(velocityDimensions);

	return clamp(targetTexCoord * uvScale, minUV, maxUV);
}

// Main function
[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	// Get dimensions and check bounds
	uint2 dimensions;
	TexColor.GetDimensions(dimensions.x, dimensions.y);

	if (DTid.x >= dimensions.x || DTid.y >= dimensions.y)
		return;

	uint2 pixelPos = DTid.xy;

	// Sample center pixel data
	float2 texCoord = (pixelPos + 0.5f) / float2(dimensions);
	float4 centerColor = TexColor.SampleLevel(LinearSampler, texCoord, 0);
	float centerDepth = TexDepth.SampleLevel(PointSampler, texCoord, 0);
	float2 centerVelocity = TexVelocity.SampleLevel(PointSampler, GetVelocityTexCoord(texCoord), 0).xy;

	centerVelocity *= Blur.VelocityParams.x;

	// Calculate tile coordinates
	uint2 gridCoord;
	gridCoord.x = (pixelPos.x * GRID_SIZE) / dimensions.x;
	gridCoord.y = (pixelPos.y * GRID_SIZE) / dimensions.y;

	uint2 neighborMaxDimensions;
	TexNeighborMax.GetDimensions(neighborMaxDimensions.x, neighborMaxDimensions.y);

	gridCoord = min(gridCoord, neighborMaxDimensions - 1);

	// Get max velocity from grid
	float4 neighborMaxSample = TexNeighborMax[gridCoord];
	float2 neighborMaxVelocity = ExtractVelocity(neighborMaxSample);

	// Determine blur direction and length
	float2 blurDir = length(neighborMaxVelocity) > 0.001f ? normalize(neighborMaxVelocity) : float2(0.0f, 0.0f);
	// Scale blur length down for higher velocity scales to prevent over-blurring
	float blurLength = min(length(neighborMaxVelocity) / sqrt(Blur.VelocityParams.x / 300.0f), g_MaxBlurRadius);

	// Skip if no motion
	if (blurLength < 0.5f) {
		RWTexOut[pixelPos] = centerColor;
		return;
	}

	float centerVelocityLen = length(centerVelocity);

	// Initialize for sampling
	float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
	uint sampleCount = uint(Blur.SampleCount);
	uint halfSampleCount = sampleCount / 2u;
	float pixelToSampleUnitsScale = float(Blur.SampleCount) / blurLength;

	// Sample in pairs (mirrored)
	for (uint i = 0; i < halfSampleCount; i++) {
		// Calculate offset
		float offset = (float(i) + 0.5f) / float(halfSampleCount) * blurLength;
		offset += GetDitheredOffset(pixelPos, i);

		// Sample pairs in opposite directions
		float2 pixelOffsetFwd = blurDir * offset;
		float2 pixelOffsetBck = -pixelOffsetFwd;

		float2 sampleTexCoordsFwd = (pixelPos + pixelOffsetFwd + 0.5f) / float2(dimensions);
		float2 sampleTexCoordsBck = (pixelPos + pixelOffsetBck + 0.5f) / float2(dimensions);

		// Sample depth and velocity
		float sampleDepthFwd = TexDepth.SampleLevel(PointSampler, sampleTexCoordsFwd, 0);
		float sampleDepthBck = TexDepth.SampleLevel(PointSampler, sampleTexCoordsBck, 0);

		float4 rawVelocityDepthFwd = TexVelocity.SampleLevel(PointSampler, GetVelocityTexCoord(sampleTexCoordsFwd), 0);
		float4 rawVelocityDepthBck = TexVelocity.SampleLevel(PointSampler, GetVelocityTexCoord(sampleTexCoordsBck), 0);

		float2 sampleVelocityFwd = rawVelocityDepthFwd.xy * Blur.VelocityParams.x;
		float2 sampleVelocityBck = rawVelocityDepthBck.xy * Blur.VelocityParams.x;

		float sampleVelocityLenFwd = length(sampleVelocityFwd);
		float sampleVelocityLenBck = length(sampleVelocityBck);

		float offsetLen = offset;

		// Calculate sample weights
		float weightFwd = SampleWeight(
			centerDepth, sampleDepthFwd, offsetLen, centerVelocityLen, sampleVelocityLenFwd,
			pixelToSampleUnitsScale, MB_SOFTZ_INCHES);

		float weightBck = SampleWeight(
			centerDepth, sampleDepthBck, offsetLen, centerVelocityLen, sampleVelocityLenBck,
			pixelToSampleUnitsScale, MB_SOFTZ_INCHES);

		// Sample colors and accumulate
		float4 sampleColorFwd = TexColor.SampleLevel(LinearSampler, sampleTexCoordsFwd, 0);
		float4 sampleColorBck = TexColor.SampleLevel(LinearSampler, sampleTexCoordsBck, 0);

		sum += weightFwd * float4(sampleColorFwd.rgb, 1.0f);
		sum += weightBck * float4(sampleColorBck.rgb, 1.0f);
	}

	// Normalize
	sum.rgb *= 1.0f / float(sampleCount);
	sum.w *= 1.0f / float(sampleCount);

	// Final blend with background
	float4 outputColor = float4(
		sum.rgb + (1.0f - sum.w) * centerColor.rgb,
		1.0f);

	// Write output
	RWTexOut[pixelPos] = outputColor;
}

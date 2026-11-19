/**
 * Motion Blur - Horizontal Reduction Pass (Pass 1a of 3)
 *
 * Performs horizontal reduction from [width x height] to [grid x height]
 * Each thread processes a horizontal strip of input pixels
 * First stage of the separable approach for maximum velocity calculation
 */

#include "Common/OrgResources.hlsli"
#include "PostProcessing/common.hlsli"

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below (MotionBlur::ReductionPassConstantBuffer at
// b0). The ORG chain (ORG/ORGMotionBlur.cpp) gives heap handles in the push block and the constants from its latch.
struct ReductionConstants
{
	float4 VelocityParams;    // x: velocity multiplier, yz: motion-vector source/display scale
	float4 TargetResolution;  // xy: output color resolution
};

// TexVelocity: full resolution motion vectors. RWTexHorizontal: the [20 x height] output.
#define HORIZONTAL_RESOURCES(X)                      \
	X(SRV, Texture2D<float4>, TexVelocity, 0)        \
	X(UAV, RWTexture2D<float4>, RWTexHorizontal, 0) \
	X(LATCH, ReductionConstants, Reduction, 0)
ORG_PUSH_BLOCK(HORIZONTAL_RESOURCES(ORG_HANDLE))
HORIZONTAL_RESOURCES(ORG_RESOURCE)

// Fixed parameters
static const uint GRID_SIZE = 20;  // Fixed grid size

uint2 GetVelocitySamplePosition(uint2 targetPixel, uint2 velocityDimensions)
{
	float2 velocityScale = Reduction.VelocityParams.yz;
	float2 sourceDimensionsF = max(float2(1.0f, 1.0f), Reduction.TargetResolution.xy * velocityScale);
	uint2 sourceDimensions = min(velocityDimensions, uint2(sourceDimensionsF + 0.5f));
	uint2 sourcePixel = uint2((float2(targetPixel) + 0.5f) * velocityScale);
	return min(sourcePixel, sourceDimensions - 1);
}

// Process horizontal strips
[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	// Get dimensions and check bounds
	uint2 velocityDimensions;
	TexVelocity.GetDimensions(velocityDimensions.x, velocityDimensions.y);

	uint2 targetDimensions = uint2(max(Reduction.TargetResolution.xy, float2(1.0f, 1.0f)) + 0.5f);

	if (DTid.y >= targetDimensions.y)
		return;

	uint2 horizontalDimensions;
	RWTexHorizontal.GetDimensions(horizontalDimensions.x, horizontalDimensions.y);

	if (DTid.x >= horizontalDimensions.x || DTid.y >= horizontalDimensions.y)
		return;

	// Calculate horizontal range to process
	uint xTileSize = (targetDimensions.x + GRID_SIZE - 1) / GRID_SIZE;
	uint xStart = DTid.x * xTileSize;
	uint xEnd = min(xStart + xTileSize, targetDimensions.x);

	// Track maximum velocity
	float maxVelocityMagnitude = 0.0f;
	float2 maxVelocity = float2(0.0f, 0.0f);

	// Process horizontal strip
	for (uint x = xStart; x < xEnd; x++) {
		// Get velocity from motion vector buffer
		uint2 velocityPixel = GetVelocitySamplePosition(uint2(x, DTid.y), velocityDimensions);
		float2 velocity = TexVelocity[velocityPixel].xy;

		// Apply velocity scale
		velocity *= Reduction.VelocityParams.x;

		// Keep largest velocity
		float velocityMagnitude = length(velocity);
		if (velocityMagnitude > maxVelocityMagnitude) {
			maxVelocityMagnitude = velocityMagnitude;
			maxVelocity = velocity;
		}
	}

	// Encode velocity for output
	// R: normalized x direction mapped to 0..1
	// G: normalized y direction mapped to 0..1
	// B: velocity magnitude scaled to 0..1
	float2 normalizedDir = maxVelocityMagnitude > 0.001f ? normalize(maxVelocity) : float2(0.0f, 0.0f);
	float encodedMagnitude = saturate(maxVelocityMagnitude * 0.05f);

	// Create output color
	float4 outputColor = float4(
		normalizedDir.x * 0.5f + 0.5f,
		normalizedDir.y * 0.5f + 0.5f,
		encodedMagnitude,
		1.0f);

	// Write to horizontal pass texture
	RWTexHorizontal[uint2(DTid.x, DTid.y)] = outputColor;
}

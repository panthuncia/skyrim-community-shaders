#include "Common/OrgResources.hlsli"

// The same layout packed tightly as in cbuffer packing, as a LATCH entry needs. Must match
// TerrainShadows::ShadowUpdateCB.
struct ShadowUpdateConstants
{
	float2 LightPxDir;   // direction on which light descends, from one pixel to next via dda
	float2 LightDeltaZ;  // per lightUVDir, normalised, [upper, lower] penumbra, should be negative
	uint StartPxCoord;
	float2 PxSize;
	float BlendWeight;
	float2 PosRange;
	float2 ZRange;
};

// D3D11: t0, u0, b0. ORG: the two textures' handles, then the latch region of this dispatch
// (Common/OrgResources.hlsli).
#define SHADOW_UPDATE_RESOURCES(X)                     \
	X(SRV, Texture2D<float>, TexHeight, 0)             \
	X(UAV, RWTexture2D<float2>, RWTexShadowHeights, 0) \
	X(LATCH, ShadowUpdateConstants, ShadowUpdate, 0)
ORG_PUSH_BLOCK(SHADOW_UPDATE_RESOURCES(ORG_HANDLE))
SHADOW_UPDATE_RESOURCES(ORG_RESOURCE)

// The heightmap's size. The textures can be larger (the render graph makes them once, at the largest heightmap's size),
// so it comes from the constants, not from GetDimensions.
uint2 GetHeightmapSize()
{
	return uint2(round(1.0 / ShadowUpdate.PxSize));
}

float2 GetInterpolatedHeightRW(float2 pxCoord, bool isVertical)
{
	uint2 dims = GetHeightmapSize();

	int2 lerpPxCoordA = int2(floor(pxCoord - .5 * float2(isVertical, !isVertical)));
	int2 lerpPxCoordB = lerpPxCoordA + int2(isVertical, !isVertical);
	lerpPxCoordA = clamp(lerpPxCoordA, int2(0, 0), int2(dims) - 1);
	lerpPxCoordB = clamp(lerpPxCoordB, int2(0, 0), int2(dims) - 1);
	float2 heightA = RWTexShadowHeights[lerpPxCoordA];
	float2 heightB = RWTexShadowHeights[lerpPxCoordB];

	return lerp(heightA, heightB, frac((isVertical ? pxCoord.x : pxCoord.y) - .5));
}

#define NTHREADS 128
groupshared float2 g_shadowHeight[NTHREADS];

// Offsets can span more than one dimension on small heightmaps, so wrap by modulo rather than a single step.
uint GetWrappedCoord(int coord, uint dimension)
{
	uint magnitude = uint(abs(coord)) % dimension;
	return coord < 0 ? (dimension - magnitude) % dimension : magnitude;
}

[numthreads(NTHREADS, 1, 1)] void main(const uint gtid : SV_GroupThreadID, const uint gid : SV_GroupID) {
	uint2 dims = GetHeightmapSize();

	bool isVertical = abs(ShadowUpdate.LightPxDir.y) > abs(ShadowUpdate.LightPxDir.x);
	int majorStep = (isVertical ? ShadowUpdate.LightPxDir.y : ShadowUpdate.LightPxDir.x) > 0.0 ? 1 : -1;
	int majorPxCoord = int(ShadowUpdate.StartPxCoord) + int(gtid) * majorStep;
	uint majorDimension = isVertical ? dims.y : dims.x;
	uint minorDimension = isVertical ? dims.x : dims.y;
	bool isValid = majorPxCoord >= 0 && majorPxCoord < int(majorDimension);
	float minorDirection = isVertical ? ShadowUpdate.LightPxDir.x : ShadowUpdate.LightPxDir.y;
	float minorOffset = 0.5 + gtid * minorDirection;
	int rawMinorPxCoord = int(gid) + int(floor(minorOffset));
	uint minorPxCoord = GetWrappedCoord(rawMinorPxCoord, minorDimension);
	uint2 outputPxCoord = isVertical ? uint2(minorPxCoord, majorPxCoord) : uint2(majorPxCoord, minorPxCoord);
	float rayWrap = floor(float(rawMinorPxCoord) / minorDimension);

	float2 pastHeights = 0.0;
	if (isValid) {
		if (ShadowUpdate.BlendWeight < 1.0)
			pastHeights = RWTexShadowHeights[outputPxCoord];

		float terrainHeight = lerp(ShadowUpdate.PosRange.x, ShadowUpdate.PosRange.y, TexHeight[outputPxCoord]);
		float2 heights = ((terrainHeight - ShadowUpdate.ZRange.x) / (ShadowUpdate.ZRange.y - ShadowUpdate.ZRange.x)).xx;

		// fetch last dispatch
		int previousMajorCoord = majorPxCoord - majorStep;
		float previousMinorCoord = gid + 0.5 - minorDirection;
		if (gtid == 0 && previousMajorCoord >= 0 && previousMajorCoord < int(majorDimension) && previousMinorCoord >= 0.0 && previousMinorCoord < minorDimension) {
			float2 previousPxCoord = isVertical ? float2(previousMinorCoord, previousMajorCoord + 0.5) : float2(previousMajorCoord + 0.5, previousMinorCoord);
			float2 sampleHeights = GetInterpolatedHeightRW(previousPxCoord, isVertical) + ShadowUpdate.LightDeltaZ;
			heights = max(heights, sampleHeights);
		}

		g_shadowHeight[gtid] = heights;
	}

	GroupMemoryBarrierWithGroupSync();

	// simple parallel scan
	[unroll] for (uint offset = 1; offset < NTHREADS; offset <<= 1)
	{
		bool combineHeights = false;
		float2 currentHeights = 0.0;
		float2 sampleHeights = 0.0;
		if (isValid && gtid >= offset) {
			int previousMinorPxCoord = int(gid) + int(floor(0.5 + (gtid - offset) * minorDirection));
			if (floor(float(previousMinorPxCoord) / minorDimension) == rayWrap) {
				combineHeights = true;
				currentHeights = g_shadowHeight[gtid];
				sampleHeights = g_shadowHeight[gtid - offset] + ShadowUpdate.LightDeltaZ * offset;
			}
		}
		GroupMemoryBarrierWithGroupSync();
		if (combineHeights) {
			g_shadowHeight[gtid] = max(currentHeights, sampleHeights);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// save
	if (isValid) {
		RWTexShadowHeights[outputPxCoord] = lerp(pastHeights, g_shadowHeight[gtid], ShadowUpdate.BlendWeight);
	}
}

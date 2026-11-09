// Copies one freshly rendered engine point-light shadow map slice into the Light Limit Fix
// local shadow cache, min-downsampling when the cache is smaller than the engine map so
// occluders never shrink.

#include "Common/OrgResources.hlsli"

struct CopyConstants
{
	uint SourceSlice;
	uint TargetSlice;
	uint Scale;  // Power of two: source texels per cache texel per axis.
	uint TargetSize;
};

#define LOCAL_SHADOW_COPY_RESOURCES(X)                  \
	X(SRV, Texture2DArray<float>, SourceShadowMaps, 0)  \
	X(UAV, RWTexture2DArray<float>, CacheShadowMaps, 0) \
	X(LATCH, CopyConstants, Copy, 0)
ORG_PUSH_BLOCK(LOCAL_SHADOW_COPY_RESOURCES(ORG_HANDLE))
LOCAL_SHADOW_COPY_RESOURCES(ORG_RESOURCE)

#define GROUP_SIZE 8

groupshared float g_scratchDepths[GROUP_SIZE][GROUP_SIZE];

// Each thread min-reduces its own footprint, then the group tree-reduces until one thread per cache texel remains.
[numthreads(GROUP_SIZE, GROUP_SIZE, 1)] void main(uint2 dispatchThreadId : SV_DispatchThreadID, uint2 groupThreadId : SV_GroupThreadID) {
	const uint footprint = max(Copy.Scale / GROUP_SIZE, 1);
	const uint threadsPerTexel = Copy.Scale / footprint;
	const uint2 sourceBase = dispatchThreadId * footprint;

	float depth = 1.0;
	for (uint y = 0; y < footprint; y++) {
		for (uint x = 0; x < footprint; x++) {
			depth = min(depth, SourceShadowMaps.Load(int4(sourceBase + uint2(x, y), Copy.SourceSlice, 0)));
		}
	}
	g_scratchDepths[groupThreadId.x][groupThreadId.y] = depth;

	for (uint stride = 1; stride < threadsPerTexel; stride *= 2) {
		GroupMemoryBarrierWithGroupSync();
		[branch] if (all(groupThreadId % (stride * 2) == 0))
		{
			float right = g_scratchDepths[groupThreadId.x + stride][groupThreadId.y];
			float bottom = g_scratchDepths[groupThreadId.x][groupThreadId.y + stride];
			float corner = g_scratchDepths[groupThreadId.x + stride][groupThreadId.y + stride];
			depth = min(min(depth, right), min(bottom, corner));
			g_scratchDepths[groupThreadId.x][groupThreadId.y] = depth;
		}
	}

	const uint2 target = sourceBase / Copy.Scale;
	if (all(groupThreadId % threadsPerTexel == 0) && all(target < Copy.TargetSize.xx))
		CacheShadowMaps[uint3(target, Copy.TargetSlice)] = depth;
}

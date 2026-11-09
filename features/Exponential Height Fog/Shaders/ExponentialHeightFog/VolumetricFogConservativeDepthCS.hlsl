// Dual-target (Common/OrgResources.hlsli). Under ORG the shared constant buffers and SharedData's depth are
// heap reads through the push block (ORG_SHARED_CONSTANTS, ORG_HEADER_RESOURCES).
#define ORG_SHARED_CONSTANTS
#define ORG_HEADER_RESOURCES
#include "Common/OrgResources.hlsli"
#include "ExponentialHeightFog/VolumetricFogConstants.hlsli"

#define VOLUMETRIC_FOG_DEPTH_RESOURCES(X)                   \
	X(SHARED, ConstantBuffer, Shared, 0)                    \
	X(LATCH, VolumetricFogConstants, VolumetricFog, 0)      \
	X(UAV, RWTexture2D<float>, ConservativeDepthTexture, 0) \
	X(EXTERN, Texture2D<float4>, DepthTexture, 17)
ORG_PUSH_BLOCK(VOLUMETRIC_FOG_DEPTH_RESOURCES(ORG_HANDLE))
VOLUMETRIC_FOG_DEPTH_RESOURCES(ORG_RESOURCE)

#include "ExponentialHeightFog/VolumetricFogCSCommon.hlsli"

void GetCellDepthCoords(uint2 cellID, out int2 minCoord, out int2 maxCoord)
{
	float2 volumeUVMin = (float2(cellID) - 0.5f.xx) * VolumetricFogInvGridSize.xy;
	float2 volumeUVMax = (float2(cellID + 1u) + 0.5f.xx) * VolumetricFogInvGridSize.xy;

	float2 eyeUVMin = saturate(volumeUVMin);
	float2 eyeUVMax = saturate(volumeUVMax);

	// The volume spans the whole view, while the depth buffer occupies the active
	// dynamic-resolution region. Include every depth pixel touched by this froxel.
	float2 renderSize = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
	float2 sampleCoordMin = min(eyeUVMin, eyeUVMax) * renderSize;
	float2 sampleCoordMax = max(eyeUVMin, eyeUVMax) * renderSize;

	minCoord = int2(floor(sampleCoordMin));
	maxCoord = int2(ceil(sampleCoordMax)) - 1;
	maxCoord = max(maxCoord, minCoord);

	int2 bufferMax = max(int2(ceil(renderSize)) - 1, int2(0, 0));
	minCoord = clamp(minCoord, int2(0, 0), bufferMax);
	maxCoord = clamp(maxCoord, int2(0, 0), bufferMax);
}

#if defined(GROUP_REDUCE)
groupshared float gsConservativeDepth[64];

[numthreads(8, 8, 1)] void main(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID, uint groupIndex : SV_GroupIndex) {
	uint2 cellID = groupID.xy;
	if (any(cellID >= VolumetricFogGridSize.xy))
		return;

	int2 minCoord, maxCoord;
	GetCellDepthCoords(cellID, minCoord, maxCoord);

	float conservativeDepth = 0.0f;
	for (int y = minCoord.y + (int)groupThreadID.y; y <= maxCoord.y; y += 8) {
		for (int x = minCoord.x + (int)groupThreadID.x; x <= maxCoord.x; x += 8) {
			float rawDepth = SharedData::DepthTexture.Load(int3(x, y, 0)).x;
			conservativeDepth = max(conservativeDepth, SharedData::GetScreenDepth(rawDepth));
		}
	}

	gsConservativeDepth[groupIndex] = conservativeDepth;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 32; stride > 0; stride >>= 1)
	{
		if (groupIndex < stride)
			gsConservativeDepth[groupIndex] = max(gsConservativeDepth[groupIndex], gsConservativeDepth[groupIndex + stride]);
		GroupMemoryBarrierWithGroupSync();
	}

	if (groupIndex == 0)
		ConservativeDepthTexture[cellID] = gsConservativeDepth[0];
}
#else
[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= VolumetricFogGridSize.xy))
		return;

	int2 minCoord, maxCoord;
	GetCellDepthCoords(dispatchID.xy, minCoord, maxCoord);

	float conservativeDepth = 0.0f;
	for (int y = minCoord.y; y <= maxCoord.y; y++) {
		for (int x = minCoord.x; x <= maxCoord.x; x++) {
			float rawDepth = SharedData::DepthTexture.Load(int3(x, y, 0)).x;
			conservativeDepth = max(conservativeDepth, SharedData::GetScreenDepth(rawDepth));
		}
	}

	ConservativeDepthTexture[dispatchID.xy] = conservativeDepth;
}
#endif

// The engine's decal occlusion test on the graph (Drawcall Limit Fix's decal occlusion family,
// src/Features/DrawcallLimitFix/Features/DecalOcclusion). The engine draws the decal's target with a BSUtilityShader silhouette pass
// (render mode 0x1B) into kDECAL_OCCLUSION, with no colour target, and counts the samples with an occlusion query. Only the depth
// matters, so this program draws the position alone: no pixel stage work, no alpha test. A draw's values are in the latch
// (DecalOcclusionDraw in DecalOcclusion.h mirrors it): the rows of the decal camera's view-projection times the target's world
// (both relative to the camera), and the geometry by buffer view ref, which the vertex stage pulls.

#include "Common/OrgResources.hlsli"

struct DecalOcclusionDraw
{
	float4 ClipRows[4];
	uint2 VertexAddress;
	uint2 IndexAddress;
	uint2 VertexLayout;
	uint VertexStride;
	uint FirstIndex;
};

#define DECAL_OCCLUSION_RESOURCES(X) X(LATCH, DecalOcclusionDraw, Draw, 0)

ORG_PUSH_BLOCK(DECAL_OCCLUSION_RESOURCES(ORG_HANDLE))
DECAL_OCCLUSION_RESOURCES(ORG_RESOURCE)

#if defined(__spirv__)
#	define DCLF_PULLED_VERTICES
#	include "Common/DCLFVertexPull.hlsli"
#endif

float4 VSMain(uint a_vertexID : SV_VertexID) : SV_Position
{
#if defined(__spirv__)
	// The draw's 16-bit index, then its position, as the input assembler reads them.
	DCLFDrawVertexLayout = Draw.VertexLayout;
	const uint indexNumber = Draw.FirstIndex + a_vertexID;
	const uint indexWord = DeviceLoad(uint, DeviceRefAdd(Draw.IndexAddress, (indexNumber * 2) & ~3u));
	const uint index = (indexNumber & 1) ? (indexWord >> 16) : (indexWord & 0xFFFF);
	DCLFStream first = { Draw.VertexAddress, Draw.VertexStride };
	DCLFStream second = { DeviceRef(0, 0), 0u };
	DCLFStreamsAt(index, first, second);
	const float4 position = float4(DCLFPullPosition(first, second).xyz, 1.0);
	return float4(dot(Draw.ClipRows[0], position), dot(Draw.ClipRows[1], position), dot(Draw.ClipRows[2], position), dot(Draw.ClipRows[3], position));
#else
	// The D3D12 build has no vertex pulling yet: it compiles, and draws nothing.
	return float4(0, 0, 0, 0);
#endif
}

void PSMain()
{
}

// Copies one mip of the filtered environment, every face, into a published cubemap: Dynamic Cubemaps'
// render-graph path (src/Features/DynamicCubemaps/ORGDynamicCubemaps.cpp). There the copy is a
// dispatch, so it can run only on the frames that publish (an indirect dispatch whose group counts
// the frame's latch region holds) while the recording is kept. The D3D11 path copies with
// CopyResource instead.

#include "Common/OrgResources.hlsli"

#define PUBLISH_CUBEMAP_RESOURCES(X)          \
	X(SRV, Texture2DArray<float4>, Source, 0) \
	X(UAV, RWTexture2DArray<float4>, Destination, 0)
ORG_PUSH_BLOCK(PUBLISH_CUBEMAP_RESOURCES(ORG_HANDLE))
PUBLISH_CUBEMAP_RESOURCES(ORG_RESOURCE)

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	uint width, height, faces;
	Destination.GetDimensions(width, height, faces);
	if (any(ThreadID.xy >= uint2(width, height)))
		return;
	Destination[ThreadID] = Source.Load(int4(ThreadID, 0));
}

#include "Common/OrgResources.hlsli"

// D3D11: u0, u1, t0, t1. ORG: four handles in the push block (Common/OrgResources.hlsli).
#define DEPTH_BLEND_RESOURCES(X)                               \
	X(UAV, RWTexture2D<float>, BlendedDepthTexture, 0)         \
	X(UAV, RWTexture2D<unorm float>, BlendedDepthTexture16, 1) \
	X(SRV, Texture2D<unorm float>, MainDepthTexture, 0)        \
	X(SRV, Texture2D<unorm float>, TerrainDepthTexture, 1)
ORG_PUSH_BLOCK(DEPTH_BLEND_RESOURCES(ORG_HANDLE))
DEPTH_BLEND_RESOURCES(ORG_RESOURCE)

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	float mixedDepth = min(MainDepthTexture[DTid.xy], TerrainDepthTexture[DTid.xy]);
	BlendedDepthTexture[DTid.xy] = mixedDepth;
	BlendedDepthTexture16[DTid.xy] = mixedDepth;
}

#include "Common/OrgResources.hlsli"

// D3D11: u0, u1, t0, t1. ORG: the handles in the push block (Common/OrgResources.hlsli).
// MASK_OUTPUT (the ORG path, ORGTerrainBlending.cpp) also stores the main depth into the terrain mask (t55). The D3D11 path copies
// the main depth into the terrain depth texture instead, which then serves as the mask. FXC cannot nest the lists, so each is whole.
#if defined(MASK_OUTPUT)
#	define DEPTH_BLEND_RESOURCES(X)                               \
		X(UAV, RWTexture2D<float>, BlendedDepthTexture, 0)         \
		X(UAV, RWTexture2D<unorm float>, BlendedDepthTexture16, 1) \
		X(UAV, RWTexture2D<float>, MaskTexture, 2)                 \
		X(SRV, Texture2D<unorm float>, MainDepthTexture, 0)        \
		X(SRV, Texture2D<unorm float>, TerrainDepthTexture, 1)
#else
#	define DEPTH_BLEND_RESOURCES(X)                               \
		X(UAV, RWTexture2D<float>, BlendedDepthTexture, 0)         \
		X(UAV, RWTexture2D<unorm float>, BlendedDepthTexture16, 1) \
		X(SRV, Texture2D<unorm float>, MainDepthTexture, 0)        \
		X(SRV, Texture2D<unorm float>, TerrainDepthTexture, 1)
#endif
ORG_PUSH_BLOCK(DEPTH_BLEND_RESOURCES(ORG_HANDLE))
DEPTH_BLEND_RESOURCES(ORG_RESOURCE)

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
#if defined(CS_ORG_BINDLESS)
	// The ORG pass dispatches the whole texture, rounded up to the group size: the threads past its edge do nothing.
	uint width, height;
	BlendedDepthTexture.GetDimensions(width, height);
	if (DTid.x >= width || DTid.y >= height)
		return;
#endif
	float mainDepth = MainDepthTexture[DTid.xy];
	float mixedDepth = min(mainDepth, TerrainDepthTexture[DTid.xy]);
	BlendedDepthTexture[DTid.xy] = mixedDepth;
	BlendedDepthTexture16[DTid.xy] = mixedDepth;
#if defined(MASK_OUTPUT)
	MaskTexture[DTid.xy] = mainDepth;
#endif
}

// Physical Glare — lossless RGB IFFT packing for vectorised upsampling

#include "Common/OrgResources.hlsli"

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below; the ORG chain (ORG/ORGPhysicalGlare.cpp)
// gives heap handles in the push block.
#define PACK_RESOURCES(X)                   \
	X(SRV, Texture2D<float2>, TexIFFT_R, 0) \
	X(SRV, Texture2D<float2>, TexIFFT_G, 1) \
	X(SRV, Texture2D<float2>, TexIFFT_B, 2) \
	X(UAV, RWTexture2D<float4>, RWTexPacked, 0)
ORG_PUSH_BLOCK(PACK_RESOURCES(ORG_HANDLE))
PACK_RESOURCES(ORG_RESOURCE)

[numthreads(8, 8, 1)] void CS_Pack(uint2 tid : SV_DispatchThreadID) {
	uint width, height;
	RWTexPacked.GetDimensions(width, height);
	if (tid.x >= width || tid.y >= height)
		return;

	RWTexPacked[tid] = float4(TexIFFT_R[tid].x, TexIFFT_G[tid].x, TexIFFT_B[tid].x, 1.0);  // Opaque alpha so the ImGui debug view isn't blended away
}

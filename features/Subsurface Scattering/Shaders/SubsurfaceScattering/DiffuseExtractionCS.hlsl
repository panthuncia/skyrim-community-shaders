// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below. The ORG passes
// (ORGSubsurfaceScattering.cpp) give heap handles in the push block, the shared constant
// buffers' too (ORG_SHARED_CONSTANTS).
#define ORG_SHARED_CONSTANTS
#include "Common/OrgResources.hlsli"

// MaskTexture: read by the Burley permutation only.
#define DIFFUSE_EXTRACTION_RESOURCES(X)         \
	X(SHARED, ConstantBuffer, Shared, 0)        \
	X(UAV, RWTexture2D<float4>, OutputRW, 0)    \
	X(SRV, Texture2D<float4>, ColorTexture, 0)  \
	X(SRV, Texture2D<float4>, MaskTexture, 2)   \
	X(SRV, Texture2D<float4>, AlbedoTexture, 3) \
	X(LATCH, SSSConstants, PerFrameSSS, 1)
ORG_PUSH_BLOCK(DIFFUSE_EXTRACTION_RESOURCES(ORG_HANDLE))

#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/SharedData.hlsli"
#include "SubsurfaceScattering/SSSCommon.hlsli"
DIFFUSE_EXTRACTION_RESOURCES(ORG_RESOURCE)

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	if (any(DTid.xy >= uint2(SharedData::BufferDim.xy)))
		return;

#if defined(BURLEY)
	if (!(MaskTexture[DTid.xy].x > 0.0))
		return;
#endif

	float4 color = ColorTexture[DTid.xy];
	float3 linearColor = ColorManagement::SceneToLinear(color.rgb);
	float3 albedo = SSSDecodeAlbedo(AlbedoTexture[DTid.xy].rgb);
	color.rgb = SSSRemoveAlbedo(linearColor, albedo, PerFrameSSS.ScatterMode);
	OutputRW[DTid.xy] = color;
}

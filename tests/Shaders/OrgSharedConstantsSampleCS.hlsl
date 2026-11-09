// Compile check for the shared constant buffers in ORG passes (ORG_SHARED_CONSTANTS): a member of each
// of FrameBuffer, SharedData, FeatureData and Permutation, an array alias, bools, and a header-declared
// (EXTERN) texture. tools/check-dual-target-shaders.py builds it for FXC (cs_5_0, the cbuffers at
// b12, b5, b6 and b4) and for DXC with CS_ORG_BINDLESS (cs_6_6, SPIR-V and DXIL, heap reads through
// the push block). It never runs.

#define ORG_SHARED_CONSTANTS
#include "Common/OrgResources.hlsli"

#define SHARED_SAMPLE_RESOURCES(X)                \
	X(SHARED, ConstantBuffer, Shared, 0)          \
	X(EXTERN, Texture2D<float4>, SharedDepth, 17) \
	X(UAV, RWTexture2D<float4>, Output, 0)
ORG_PUSH_BLOCK(SHARED_SAMPLE_RESOURCES(ORG_HANDLE))

// A header's resource, read through the EXTERN handle above under ORG.
namespace SharedSample
{
	ORG_RESOURCE(SRV, Texture2D<float4>, SharedDepth, 17)
}

#include "Common/Permutation.hlsli"
#include "Common/SharedData.hlsli"

SHARED_SAMPLE_RESOURCES(ORG_RESOURCE)

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= uint2(SharedData::BufferDim.xy)))
		return;
	float4 colour = mul(FrameBuffer::CameraViewProj, float4(id.xy * SharedData::BufferDim.zw, 0, 1));
	colour += SharedData::WaterData[id.x % 25];
	colour *= SharedData::iblSettings.EnvIBLScale;
	colour += SharedSample::SharedDepth[id.xy];
	if (SharedData::InInterior || SharedData::extendedMaterialSettings.EnableParallax)
		colour *= 0.5;
	if (Permutation::PixelShaderDescriptor & 1)
		colour = 0;
	Output[id.xy] = colour;
}

#if defined(CS_ORG_BINDLESS)
// The ORG build (Features/VolumetricLighting/ORGVolumetricLighting): the volume from the heap, and PerTechnique (the engine's
// block: the volume's size, then the step) from the port's latch.
#	include "Common/OrgResources.hlsli"

struct VolumetricLightingRaymarchConstants
{
	float3 TextureDimensions;
	int StepIndex;
};

#	define VL_RAYMARCH_RESOURCES(X)                    		X(UAV, RWTexture3D<float>, DensityRW, 0) 		X(LATCH, VolumetricLightingRaymarchConstants, PerTechnique, 0)
ORG_PUSH_BLOCK(VL_RAYMARCH_RESOURCES(ORG_HANDLE))
VL_RAYMARCH_RESOURCES(ORG_RESOURCE)

static float3 TextureDimensions = PerTechnique.TextureDimensions;
#else
SamplerState DensitySampler : register(s0);
RWTexture3D<float> DensityRW : register(u0);

cbuffer PerTechnique : register(b0)
{
	float3 TextureDimensions : packoffset(c0);
	int StepIndex : packoffset(c1);
}
#endif

[numthreads(32, 32, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	uint depth = (uint)TextureDimensions.z;
	uint3 currCoord = uint3(dispatchID.xy, 0);
	float acc = DensityRW[currCoord];
	for (currCoord.z = 1; currCoord.z < depth; currCoord.z++) {
		acc += DensityRW[currCoord];
		DensityRW[currCoord] = acc;
	}
}

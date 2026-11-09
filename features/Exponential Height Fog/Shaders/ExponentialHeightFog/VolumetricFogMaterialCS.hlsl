// Dual-target (Common/OrgResources.hlsli). Under ORG the shared constant buffers and SharedData's depth are
// heap reads through the push block (ORG_SHARED_CONSTANTS, ORG_HEADER_RESOURCES).
#define ORG_SHARED_CONSTANTS
#define ORG_HEADER_RESOURCES
#include "Common/OrgResources.hlsli"
#include "ExponentialHeightFog/VolumetricFogConstants.hlsli"

#define VOLUMETRIC_FOG_MATERIAL_RESOURCES(X)           \
	X(SHARED, ConstantBuffer, Shared, 0)               \
	X(LATCH, VolumetricFogConstants, VolumetricFog, 0) \
	X(UAV, RWTexture3D<float4>, VBufferA, 0)           \
	X(EXTERN, Texture2D<float4>, DepthTexture, 17)
ORG_PUSH_BLOCK(VOLUMETRIC_FOG_MATERIAL_RESOURCES(ORG_HANDLE))
VOLUMETRIC_FOG_MATERIAL_RESOURCES(ORG_RESOURCE)

#include "ExponentialHeightFog/VolumetricFogCSCommon.hlsli"

[numthreads(8, 8, 4)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (!ExponentialHeightFog::IsInsideVolumetricGrid(dispatchID))
		return;

	float viewDepth;
	float3 positionWS = ExponentialHeightFog::ComputeCellWorldPosition(dispatchID, 0.5f.xxx, viewDepth);

	float extinction = ExponentialHeightFog::EvaluateHeightFogExtinction(positionWS, FrameBuffer::CameraPosAdjust.xyz);
	float3 albedo = saturate(SharedData::exponentialHeightFogSettings.volumetricFogAlbedo.rgb);
	float3 scattering = extinction * albedo * SharedData::exponentialHeightFogSettings.volumetricFogAlbedo.a;

	VBufferA[dispatchID] = float4(scattering, extinction);
}

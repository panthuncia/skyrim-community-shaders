// Dual-target (Common/OrgResources.hlsli). Under ORG the shared constant buffers and SharedData's depth are
// heap reads through the push block (ORG_SHARED_CONSTANTS, ORG_HEADER_RESOURCES).
#define ORG_SHARED_CONSTANTS
#define ORG_HEADER_RESOURCES
#include "Common/OrgResources.hlsli"
#include "ExponentialHeightFog/VolumetricFogConstants.hlsli"

#define VOLUMETRIC_FOG_INTEGRATION_RESOURCES(X)               \
	X(SHARED, ConstantBuffer, Shared, 0)                      \
	X(LATCH, VolumetricFogConstants, VolumetricFog, 0)        \
	X(SRV, Texture3D<float4>, LightScattering, 0)             \
	X(UAV, RWTexture3D<float4>, IntegratedLightScattering, 0) \
	X(EXTERN, Texture2D<float4>, DepthTexture, 17)
ORG_PUSH_BLOCK(VOLUMETRIC_FOG_INTEGRATION_RESOURCES(ORG_HANDLE))
VOLUMETRIC_FOG_INTEGRATION_RESOURCES(ORG_RESOURCE)

#include "ExponentialHeightFog/VolumetricFogCSCommon.hlsli"

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= VolumetricFogGridSize.xy))
		return;

	float3 accumulatedLighting = 0.0f.xxx;
	float accumulatedTransmittance = 1.0f;
	float accumulatedDepth = 0.0f;

	float previousDepth;
	float3 previousPositionWS = ExponentialHeightFog::ComputeCellWorldPosition(uint3(dispatchID.xy, 0), float3(0.5f, 0.5f, 0.0f), previousDepth);
#if defined(VOLUMETRIC_FOG_FAR_GRID)
	float nearGridStartDepth = (1.0f - VolumetricFogGridZParams.y) / max(VolumetricFogGridZParams.x, 1e-20f);
	accumulatedDepth = length(previousPositionWS) * max(previousDepth - nearGridStartDepth, 0.0f) / max(previousDepth, 1e-5f);
#endif

	[loop] for (uint layerIndex = 0; layerIndex < VolumetricFogGridSize.z; layerIndex++)
	{
		uint3 layerCoordinate = uint3(dispatchID.xy, layerIndex);
		float4 scatteringAndExtinction = LightScattering[layerCoordinate];

		float layerDepth;
		float3 layerPositionWS = ExponentialHeightFog::ComputeCellWorldPosition(layerCoordinate, 0.5f.xxx, layerDepth);
		float stepLength = length(layerPositionWS - previousPositionWS);
		previousPositionWS = layerPositionWS;

		float extinction = max(scatteringAndExtinction.w, 0.0f);
		float transmittance = exp(-extinction * stepLength);

		accumulatedDepth += stepLength;
		float fadeIn = saturate(accumulatedDepth * VolumetricFogNearFadeInDistanceInv);

		float3 scatteringIntegratedOverSlice =
			fadeIn * (scatteringAndExtinction.rgb - scatteringAndExtinction.rgb * transmittance) / max(extinction, 1e-5f);
		accumulatedLighting += scatteringIntegratedOverSlice * accumulatedTransmittance;
		accumulatedTransmittance *= lerp(1.0f, transmittance, fadeIn);

		IntegratedLightScattering[layerCoordinate] = float4(max(accumulatedLighting, 0.0f.xxx), saturate(accumulatedTransmittance));
	}
}

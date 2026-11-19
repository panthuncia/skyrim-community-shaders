#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0-t4 and s0-s4 from the heap, the shared
// constants, and PerGeometry from the engine-constants capture through the pass's latch.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISWATERBLEND_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, sourceTex, 0) X(SRV, Texture2D<float4>, waterHistoryTex, 1) X(SRV, Texture2D<float4>, motionBufferTex, 2) X(SRV, Texture2D<float4>, depthBufferTex, 3) X(SRV, Texture2D<float4>, waterMaskTex, 4) X(SAMPLER, SamplerState, sourceSampler, 0) X(SAMPLER, SamplerState, waterHistorySampler, 1) X(SAMPLER, SamplerState, motionBufferSampler, 2) X(SAMPLER, SamplerState, depthBufferSampler, 3) X(SAMPLER, SamplerState, waterMaskSampler, 4) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISWATERBLEND_ORG_RESOURCES(ORG_HANDLE))
ISWATERBLEND_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float3 Color: SV_Target0;
	float4 Color1: SV_Target1;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
static float4 NearFar_Menu_DistanceFactor = OrgEngineRow(PerGeometryAt, 0);
#	else
SamplerState sourceSampler : register(s0);
SamplerState waterHistorySampler : register(s1);
SamplerState motionBufferSampler : register(s2);
SamplerState depthBufferSampler : register(s3);
SamplerState waterMaskSampler : register(s4);

Texture2D<float4> sourceTex : register(t0);
Texture2D<float4> waterHistoryTex : register(t1);
Texture2D<float4> motionBufferTex : register(t2);
Texture2D<float4> depthBufferTex : register(t3);
Texture2D<float4> waterMaskTex : register(t4);

cbuffer PerGeometry : register(b2)
{
	float4 NearFar_Menu_DistanceFactor : packoffset(c0);
};
#	endif

namespace WaterBlend
{
	static const float WaterMaskThreshold = 1e-4f;
	static const float FullWaterCoverageThreshold = 1e-3f;

	float GetWaterCoverage(float waterMask)
	{
		return saturate((waterMask - WaterMaskThreshold) / (FullWaterCoverageThreshold - WaterMaskThreshold));
	}
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	float2 adjustedScreenPosition = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);
	float waterMask = waterMaskTex.Sample(waterMaskSampler, adjustedScreenPosition).z;
	if (waterMask < WaterBlend::WaterMaskThreshold) {
		discard;
	}

	float3 sourceColor = sourceTex.Sample(sourceSampler, adjustedScreenPosition).xyz;
	float2 motion = motionBufferTex.Sample(motionBufferSampler, adjustedScreenPosition).xy;
	float2 motionScreenPosition = input.TexCoord + motion;
	float2 motionAdjustedScreenPosition =
		FrameBuffer::GetPreviousDynamicResolutionAdjustedScreenPosition(motionScreenPosition);
	float4 waterHistory =
		waterHistoryTex.Sample(waterHistorySampler, motionAdjustedScreenPosition).xyzw;

	float3 finalColor = sourceColor;
	if (motionScreenPosition.x >= 0 && motionScreenPosition.y >= 0 && motionScreenPosition.x <= 1 &&
		motionScreenPosition.y <= 1 && waterHistory.w > 0.0 && all(isfinite(waterHistory))) {
		float historyFactor = 0.95;
		if (NearFar_Menu_DistanceFactor.z == 0) {
			float depth = depthBufferTex.Sample(depthBufferSampler, adjustedScreenPosition).x;
			float distanceFactor = clamp(250 * ((-NearFar_Menu_DistanceFactor.x +
													(2 * NearFar_Menu_DistanceFactor.x * NearFar_Menu_DistanceFactor.y) /
														(-(depth * 2 - 1) *
																(NearFar_Menu_DistanceFactor.y - NearFar_Menu_DistanceFactor.x) +
															(NearFar_Menu_DistanceFactor.y + NearFar_Menu_DistanceFactor.x))) /
												   (NearFar_Menu_DistanceFactor.y - NearFar_Menu_DistanceFactor.x)),
				0.1, 0.95);
			historyFactor = NearFar_Menu_DistanceFactor.w * (distanceFactor * (waterMask * -0.85 + 0.95));
		}
		// Un-premultiply history so bilinear filtering against cleared pixels does not darken water edges
		float3 historyColor = waterHistory.xyz / max(waterHistory.w, EPSILON_DIVISION);

		historyFactor *= waterHistory.w;
		finalColor = lerp(sourceColor, historyColor, historyFactor);
	}

	float waterCoverage = WaterBlend::GetWaterCoverage(waterMask);
	// Store premultiplied history so transparent clears filter without dark outlines
	psout.Color1 = float4(finalColor * waterCoverage, waterCoverage);
	psout.Color = finalColor;

	return psout;
}
#endif

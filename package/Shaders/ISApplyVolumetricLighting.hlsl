#if defined(CS_ORG_BINDLESS)
// The ORG build (Features/VolumetricLighting/ORGVolumetricLighting): the textures and samplers from the heap, FrameBuffer and
// SharedData from the shared constants, PerGeometry (the engine's values, and the depth repartition's exponent) from the port's
// latch. The repartition texture is computed (Common/VolumetricLightingRepartition.hlsli).
#	define ORG_SHARED_CONSTANTS
#	include "Common/OrgResources.hlsli"
#	include "Common/VolumetricLightingRepartition.hlsli"

struct VolumetricLightingApplyConstants
{
	float4 IntensityTemporal;  // g_IntensityX_TemporalY
	float4 Repartition;        // x: the repartition's exponent
};

#	define VL_APPLY_RESOURCES(X)                                  		X(SHARED, ConstantBuffer, Shared, 0)                       		X(SRV, Texture2D<float4>, DepthTex, 0)                     		X(SRV, Texture2D<float4>, PreviousFrameTex, 1)             		X(SRV, Texture3D<float4>, VLTex, 2)                        		X(SRV, Texture2D<float4>, NoiseGradSamplerTex, 4)          		X(SRV, Texture2D<float4>, MotionVectorsTex, 5)             		X(SRV, Texture2D<float4>, PreviousDepthTex, 6)             		X(SAMPLER, SamplerState, DepthSampler, 0)                  		X(SAMPLER, SamplerState, PreviousFrameSampler, 1)          		X(SAMPLER, SamplerState, VLSampler, 2)                     		X(SAMPLER, SamplerState, NoiseGradSamplerSampler, 4)       		X(SAMPLER, SamplerState, MotionVectorsSampler, 5)          		X(SAMPLER, SamplerState, PreviousDepthSampler, 6)          		X(LATCH, VolumetricLightingApplyConstants, PerGeometry, 2)
ORG_PUSH_BLOCK(VL_APPLY_RESOURCES(ORG_HANDLE))
VL_APPLY_RESOURCES(ORG_RESOURCE)

static float4 g_IntensityX_TemporalY = PerGeometry.IntensityTemporal;
#	define VL_REPARTITION(U) VolumetricLightingRepartition::Sample(U, PerGeometry.Repartition.x)
#endif

#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float VL: SV_Target0;
};

#if defined(PSHADER)
#	if !defined(CS_ORG_BINDLESS)
SamplerState DepthSampler : register(s0);
SamplerState PreviousFrameSampler : register(s1);
SamplerState VLSampler : register(s2);
SamplerState RepartitionSampler : register(s3);
SamplerState NoiseGradSamplerSampler : register(s4);
SamplerState MotionVectorsSampler : register(s5);
SamplerState PreviousDepthSampler : register(s6);

Texture2D<float4> DepthTex : register(t0);
Texture2D<float4> PreviousFrameTex : register(t1);
Texture3D<float4> VLTex : register(t2);
Texture1D<float4> RepartitionTex : register(t3);
Texture2D<float4> NoiseGradSamplerTex : register(t4);
Texture2D<float4> MotionVectorsTex : register(t5);
Texture2D<float4> PreviousDepthTex : register(t6);

cbuffer PerGeometry : register(b2)
{
	float4 g_IntensityX_TemporalY : packoffset(c0);
};

#		define VL_REPARTITION(U) RepartitionTex.SampleLevel(RepartitionSampler, U, 0).x
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	static const float kVLThresholdBias = 1.0 / 128.0;

#	if defined(CS_ORG_BINDLESS)
	// The pass draws the whole target. The engine's draw covers only its dynamic-resolution part, over which TexCoord runs
	// from 0 to 1: the same pixels and coordinates.
	input.TexCoord *= FrameBuffer::DynamicResolutionParams2.xy;
	if (any(input.TexCoord >= 1.0))
		discard;
#	endif

	float2 screenPosition = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);
	float depth = DepthTex.Sample(DepthSampler, screenPosition).x;

	float repartition = clamp(VL_REPARTITION(depth), 0, 0.9999);
	float vl = g_IntensityX_TemporalY.x * VLTex.SampleLevel(VLSampler, float3(input.TexCoord, repartition), 0).x;

	const bool dither = !SharedData::enbSettings.Enable || !SharedData::enbSettings.GameVolumetricRaysDisableDithering;
	float noiseGrad = dither ? 0.03125 * NoiseGradSamplerTex.Sample(NoiseGradSamplerSampler, 0.125 * input.Position.xy).x : 0.0;

	float adjustedVl = max(0.0, noiseGrad + vl - kVLThresholdBias);

	if (0.001 < g_IntensityX_TemporalY.y) {
		float2 motionVector = MotionVectorsTex.Sample(MotionVectorsSampler, screenPosition).xy;
		float2 previousTexCoord = input.TexCoord + motionVector;
		bool isValid = previousTexCoord.x >= 0 && previousTexCoord.x < 1 && previousTexCoord.y >= 0 && previousTexCoord.y < 1;
		float2 previousScreenPosition = FrameBuffer::GetPreviousDynamicResolutionAdjustedScreenPosition(previousTexCoord);
		float previousVl = PreviousFrameTex.Sample(PreviousFrameSampler, previousScreenPosition).x;
		float previousDepth = PreviousDepthTex.Sample(PreviousDepthSampler, previousScreenPosition).x;

		float temporalContribution = g_IntensityX_TemporalY.y * (1 - smoothstep(0, 1, min(1, 100 * abs(depth - previousDepth))));
		psout.VL = lerp(adjustedVl, previousVl, temporalContribution * isValid);
	} else {
		psout.VL = adjustedVl;
	}

	return psout;
}
#endif

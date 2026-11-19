#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0-t2 and s0-s2 from the heap,
// FrameBuffer from the captured PerFrame, PerGeometry from the engine-constants capture through the pass's latch.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISDOWNSAMPLE_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, SourceTex, 0) X(SRV, Texture2D<float4>, PreviousFrameSourceTex, 1) X(SRV, Texture2D<float4>, MotionVectorsTex, 2) X(SAMPLER, SamplerState, SourceSampler, 0) X(SAMPLER, SamplerState, PreviousFrameSourceSampler, 1) X(SAMPLER, SamplerState, MotionVectorsSampler, 2) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISDOWNSAMPLE_ORG_RESOURCES(ORG_HANDLE))
ISDOWNSAMPLE_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/Color.hlsli"
#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
// c0 packs TexelSize (xy), SamplesCount (z, read with asuint) and CompensateJittering (w, a bool: nonzero bits).
static float4 OrgRow0 = OrgEngineRow(PerGeometryAt, 0);
static float2 TexelSize = OrgRow0.xy;
static float SamplesCount = OrgRow0.z;
static bool CompensateJittering = asuint(OrgRow0.w) != 0;
static float4 OffsetsAndWeights[16] = ORG_ENGINE_ROWS16(PerGeometryAt, 1);
#	else
SamplerState SourceSampler : register(s0);
SamplerState PreviousFrameSourceSampler : register(s1);
SamplerState MotionVectorsSampler : register(s2);

Texture2D<float4> SourceTex : register(t0);
Texture2D<float4> PreviousFrameSourceTex : register(t1);
Texture2D<float4> MotionVectorsTex : register(t2);

cbuffer PerGeometry : register(b2)
{
	float2 TexelSize : packoffset(c0);
	float SamplesCount : packoffset(c0.z);
	bool CompensateJittering : packoffset(c0.w);
	float4 OffsetsAndWeights[16] : packoffset(c1);
};
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float4 downsampledColor = 0;
	float luminance = 0;
	for (uint sampleIndex = 0; sampleIndex < asuint(SamplesCount); ++sampleIndex) {
		float2 texCoord = OffsetsAndWeights[sampleIndex].xy * TexelSize.xy + input.TexCoord;
#	if defined(DYNAMIC_SOURCE)
		texCoord = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(texCoord);
#	endif
		float4 sourceColor = SourceTex.Sample(SourceSampler, texCoord);
#	if defined(DYNAMIC_SOURCE)
		downsampledColor += sourceColor;
#	else
		float sampleLuminance = Color::RGBToLuminanceAlternative(sourceColor.xyz);
		if (sampleLuminance > luminance) {
			downsampledColor = sourceColor;
			luminance = sampleLuminance;
		}
#	endif
	}
#	if defined(DYNAMIC_SOURCE)
	psout.Color = downsampledColor / asuint(SamplesCount);
#	else
	if (CompensateJittering) {
		float2 adjustedTexCoord = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);
		float2 motion = MotionVectorsTex.Sample(MotionVectorsSampler, adjustedTexCoord).xy;
		float4 previousFrameColor =
			PreviousFrameSourceTex.Sample(PreviousFrameSourceSampler, input.TexCoord + motion).xyzw;
		psout.Color = 0.5 * (previousFrameColor + downsampledColor);
	} else {
		psout.Color = downsampledColor;
	}
#	endif

	return psout;
}
#endif

#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0 and s0 from the heap, FrameBuffer
// from the captured PerFrame, PerGeometry from the engine-constants capture through the pass's latch.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISSAOMINIFY_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, sourceTex, 0) X(SAMPLER, SamplerState, sourceSampler, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISSAOMINIFY_ORG_RESOURCES(ORG_HANDLE))
ISSAOMINIFY_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
static float4 g_RenderTargetResolution = OrgEngineRow(PerGeometryAt, 0);
static float4 g_ContrastParams = OrgEngineRow(PerGeometryAt, 1);
static float g_UseDynamicSampling = OrgEngineRow(PerGeometryAt, 2).x;
#	else
SamplerState sourceSampler : register(s0);

Texture2D<float4> sourceTex : register(t0);

cbuffer PerGeometry : register(b2)
{
	float4 g_RenderTargetResolution : packoffset(c0);
	float4 g_ContrastParams : packoffset(c1);
	float g_UseDynamicSampling : packoffset(c2);
};
#	endif

float2 GetMinifiedTexCoord(float2 texCoord)
{
	return ((float2)(((int2)(g_RenderTargetResolution.yx * texCoord.yx) & 1) ^ 1) * 2 - 1) *
	           g_RenderTargetResolution.zw +
	       texCoord;
}

static const float ContrastValues[] = { 0.300000, 0.400000,
	0.500000, 0.400000, 0.300000, 0.400000,
	2.000000, 2.500000, 2.000000, 0.400000,
	0.500000, 2.500000, 3.500000, 2.500000,
	0.500000, 0.400000, 2.000000, 2.500000,
	2.000000, 0.400000, 0.300000, 0.400000,
	0.500000, 0.400000, 0.300000 };

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 finalTexCoord;
	if (asuint(g_UseDynamicSampling) > 0) {
		float2 drAdjustedTexCoord = FrameBuffer::DynamicResolutionParams1.xy * input.TexCoord;
		float2 minifiedTexCoord = GetMinifiedTexCoord(drAdjustedTexCoord);
		finalTexCoord = clamp(minifiedTexCoord, 0,
			FrameBuffer::DynamicResolutionParams1.xy - float2(FrameBuffer::CameraPreviousPosAdjust.w, 0));
	} else {
		finalTexCoord = GetMinifiedTexCoord(input.TexCoord);
	}

	float4 color = 0;
	if (max(g_RenderTargetResolution.x, g_RenderTargetResolution.y) == 3.0) {
		float2 sampleOffset =
			(((int2)g_RenderTargetResolution.xy & 1) * (1. / 6.) + 0.5) /
			g_RenderTargetResolution.xy;
		color.x = sourceTex.Sample(sourceSampler, finalTexCoord + sampleOffset).x;
		color.x += sourceTex.Sample(sourceSampler,
								finalTexCoord + float2(-sampleOffset.x, sampleOffset.y))
		               .x;
		color.x += sourceTex.Sample(sourceSampler, finalTexCoord - sampleOffset).x;
		color.x += sourceTex.Sample(sourceSampler,
								finalTexCoord + float2(sampleOffset.x, -sampleOffset.y))
		               .x;
		color.x *= 0.25;
	} else {
		color = sourceTex.Sample(sourceSampler, finalTexCoord);
	}

#	if defined(APPLY_CENTER_CONTRAST)
	int contrastIndex = (int)(5 * input.TexCoord.x) + (int)(5 * input.TexCoord.y) * 5;
	float contrastFactor = ContrastValues[contrastIndex] * g_ContrastParams.x;
	color *= contrastFactor;
#	endif

	psout.Color = color;

	return psout;
}
#endif

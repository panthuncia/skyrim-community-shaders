#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0 and s0 from the heap, the shared
// constants, and PerGeometry from the engine-constants capture through the pass's latch. The vertex stage is the graph's
// fullscreen triangle.
#	define ORG_SHARED_CONSTANTS
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISUPSAMPLEDYNAMICRESOLUTION_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, sourceTex, 0) X(SAMPLER, SamplerState, sourceSampler, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISUPSAMPLEDYNAMICRESOLUTION_ORG_RESOURCES(ORG_HANDLE))
ISUPSAMPLEDYNAMICRESOLUTION_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/DummyVSTexCoord.hlsl"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
static float4 g_UpsampleParameters = OrgEngineRow(PerGeometryAt, 0);
#	else
SamplerState sourceSampler : register(s0);

Texture2D<float4> sourceTex : register(t0);

cbuffer PerGeometry : register(b2)
{
	float4 g_UpsampleParameters : packoffset(c0);
};
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 upsampleTexCoord = min(g_UpsampleParameters.zw, g_UpsampleParameters.xy * input.TexCoord);
	psout.Color = sourceTex.Sample(sourceSampler, upsampleTexCoord).xyzw;

	return psout;
}
#endif

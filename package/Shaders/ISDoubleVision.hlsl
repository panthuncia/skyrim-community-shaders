#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0-t1 and s0-s1 from the heap, the shared
// constants, and PerGeometry from the engine-constants capture through the pass's latch.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISDOUBLEVISION_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, Src0Tex, 0) X(SRV, Texture2D<float4>, Src1Tex, 1) X(SAMPLER, SamplerState, Src0Sampler, 0) X(SAMPLER, SamplerState, Src1Sampler, 1) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISDOUBLEVISION_ORG_RESOURCES(ORG_HANDLE))
ISDOUBLEVISION_ORG_RESOURCES(ORG_RESOURCE)
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
static float4 blurParams = OrgEngineRow(PerGeometryAt, 0);
static float4 doubleVisParams = OrgEngineRow(PerGeometryAt, 1);
#	else
SamplerState Src0Sampler : register(s0);
SamplerState Src1Sampler : register(s1);

Texture2D<float4> Src0Tex : register(t0);
Texture2D<float4> Src1Tex : register(t1);

cbuffer PerGeometry : register(b2)
{
	float4 blurParams : packoffset(c0);
	float4 doubleVisParams : packoffset(c1);
};
#	endif

float2 GetSourceTexCoord(float2 texCoord)
{
	return blurParams.x > 0.5 ? FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(texCoord) : texCoord;
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 texCoord0 = float2(max(0, -doubleVisParams.x + input.TexCoord.x),
		max(-doubleVisParams.y + input.TexCoord.y, 1 - doubleVisParams.w));
	float2 adjustedTexCoord0 = GetSourceTexCoord(texCoord0);
	float3 src0Color0 = Src0Tex.Sample(Src0Sampler, adjustedTexCoord0).xyz;

	float2 texCoord1 = float2(min(doubleVisParams.z, doubleVisParams.x + input.TexCoord.x),
		min(1, doubleVisParams.y + input.TexCoord.y));
	float2 adjustedTexCoord1 = GetSourceTexCoord(texCoord1);
	float3 src0Color1 = Src0Tex.Sample(Src0Sampler, adjustedTexCoord1).xyz;

	float2 adjustedTexCoord2 = GetSourceTexCoord(input.TexCoord);
	float3 src1Color = Src1Tex.Sample(Src1Sampler, adjustedTexCoord2).xyz;

	float src1Factor = min(1,
		blurParams.z *
			length(float2(2 * ((doubleVisParams.z / doubleVisParams.w) * (input.TexCoord.x - 0.5)),
				2 * (input.TexCoord.y - 0.5))));

	float3 color = 0.5 * ((1 - src1Factor) * (src0Color0 + src0Color1)) + src1Factor * src1Color;

	psout.Color = float4(color, 1);

	return psout;
}
#endif

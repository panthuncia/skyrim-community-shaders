#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0-t1 and s0-s1 from the heap, the shared
// constants, and PerGeometry from the engine-constants capture through the pass's latch.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISREFRACTION_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, Src0Tex, 0) X(SRV, Texture2D<float4>, Src1Tex, 1) X(SAMPLER, SamplerState, Src0Sampler, 0) X(SAMPLER, SamplerState, Src1Sampler, 1) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISREFRACTION_ORG_RESOURCES(ORG_HANDLE))
ISREFRACTION_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/Color.hlsli"
#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
static float4 Tint = OrgEngineRow(PerGeometryAt, 0);
#	else
SamplerState Src0Sampler : register(s0);
SamplerState Src1Sampler : register(s1);

Texture2D<float4> Src0Tex : register(t0);
Texture2D<float4> Src1Tex : register(t1);

cbuffer PerGeometry : register(b2)
{
	float4 Tint : packoffset(c0);
};
#	endif

float2 GetRefractedTexCoord(float2 texCoordOriginal, float3 normalOriginal)
{
	float2 texCoord = texCoordOriginal + float2(-1, 1) * (2 * (0.05 * normalOriginal.z) * (normalOriginal.xy - 0.5));
	float2 texCoordClamped = texCoord > 0.85 ? lerp(0.85, texCoord, 0.78) : texCoord;
	texCoordClamped = texCoord < 0.15 ? lerp(0.15, texCoord, 0.78) : texCoordClamped;
	return FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(lerp(texCoord, texCoordClamped, normalOriginal.z));
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 texCoordOriginal = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);

	float4 normalOriginal = Src1Tex.Sample(Src0Sampler, texCoordOriginal);

	float4 colorOriginal = Src0Tex.Sample(Src0Sampler, texCoordOriginal);

	float2 texCoordRefracted = GetRefractedTexCoord(input.TexCoord, normalOriginal.xyz);

	float refractedMask = Src1Tex.Sample(Src0Sampler, texCoordRefracted).w;
	float4 colorRefracted = Src0Tex.Sample(Src0Sampler, texCoordRefracted);
	float4 colorResulting = lerp(colorOriginal, colorRefracted, refractedMask);

	psout.Color.xyz = colorResulting.xyz;
	psout.Color.w = colorResulting.w;

	return psout;
}
#endif

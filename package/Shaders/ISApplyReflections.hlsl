#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0-t2 and s0-s2 from the heap, the shared
// constants, and PerGeometry from the engine-constants capture through the pass's latch. The vertex stage is the graph's
// fullscreen triangle.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISAPPLYREFLECTIONS_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, SSRSourceTex, 0) X(SRV, Texture2D<float4>, WaterMaskTex, 1) X(SRV, Texture2D<float4>, MainBufferTex, 2) X(SAMPLER, SamplerState, SSRSourceSampler, 0) X(SAMPLER, SamplerState, WaterMaskSampler, 1) X(SAMPLER, SamplerState, MainBufferSampler, 2) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISAPPLYREFLECTIONS_ORG_RESOURCES(ORG_HANDLE))
ISAPPLYREFLECTIONS_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float3 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
static float4 SSRParams = OrgEngineRow(PerGeometryAt, 0);
#	else
SamplerState SSRSourceSampler : register(s0);
SamplerState WaterMaskSampler : register(s1);
SamplerState MainBufferSampler : register(s2);

Texture2D<float4> SSRSourceTex : register(t0);
Texture2D<float4> WaterMaskTex : register(t1);
Texture2D<float4> MainBufferTex : register(t2);

cbuffer PerGeometry : register(b2)
{
	float4 SSRParams : packoffset(c0);
};
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 adjustedScreenPosition = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord.xy);

	float2 waterMask = WaterMaskTex.SampleLevel(WaterMaskSampler, adjustedScreenPosition, 0).zw;
	float3 mainColor = MainBufferTex.Sample(MainBufferSampler, adjustedScreenPosition).xyz;

	float3 colorOffset = 0.0.xxx;
	if (1e-5 >= waterMask.x && waterMask.y > 1e-5) {
		float4 ssrSourceColor = SSRSourceTex.Sample(SSRSourceSampler, adjustedScreenPosition);
		colorOffset = clamp(SSRParams.x * (ssrSourceColor.xyz * ssrSourceColor.w),
			0, SSRParams.y * mainColor);
	}

	psout.Color = colorOffset + mainColor;

	return psout;
}
#endif

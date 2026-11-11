#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0 and s0 from the heap, FrameBuffer
// from the captured PerFrame, PerGeometry from the engine-constants capture through the pass's latch.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISSAOCAMERAZ_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, DepthTex, 0) X(SAMPLER, SamplerState, DepthSampler, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISSAOCAMERAZ_ORG_RESOURCES(ORG_HANDLE))
ISSAOCAMERAZ_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float ClippedDepth: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
static float4 g_ClipInfos = OrgEngineRow(PerGeometryAt, 0);
static float4 g_ScreenInfos = OrgEngineRow(PerGeometryAt, 1);
#	else
SamplerState DepthSampler : register(s0);

Texture2D<float4> DepthTex : register(t0);

cbuffer PerGeometry : register(b2)
{
	float4 g_ClipInfos : packoffset(c0);
	float4 g_ScreenInfos : packoffset(c1);
};
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 screenPosition = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);
	float depth = DepthTex.Sample(DepthSampler, screenPosition).x;

	psout.ClippedDepth = clamp(g_ClipInfos.x / (g_ClipInfos.y * depth + g_ClipInfos.z), 0, g_ClipInfos.w);

	return psout;
}
#endif

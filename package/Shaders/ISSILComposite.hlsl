#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0-t1 and s0-s1 from the heap, the shared
// constants, and PerGeometry from the engine-constants capture through the pass's latch. The vertex stage is the graph's
// fullscreen triangle.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISSILCOMPOSITE_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, sourceTex, 0) X(SRV, Texture2D<float4>, IndLightingTex, 1) X(SAMPLER, SamplerState, sourceSampler, 0) X(SAMPLER, SamplerState, IndLightingSampler, 1) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISSILCOMPOSITE_ORG_RESOURCES(ORG_HANDLE))
ISSILCOMPOSITE_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if !defined(CS_ORG_BINDLESS)
SamplerState sourceSampler : register(s0);
SamplerState IndLightingSampler : register(s1);

Texture2D<float4> sourceTex : register(t0);
Texture2D<float4> IndLightingTex : register(t1);
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float4 sourceColor = sourceTex.Sample(sourceSampler, input.TexCoord);
	float4 ilColor = IndLightingTex.Sample(IndLightingSampler, input.TexCoord);

	psout.Color = sourceColor + ilColor;

	return psout;
}
#endif

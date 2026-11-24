#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0 and s0 from the heap, the shared
// constants, and PerGeometry from the engine-constants capture through the pass's latch. The vertex stage is the graph's
// fullscreen triangle over the 16x1 target.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define LENSFLARE_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, DepthTex, 0) X(SAMPLER, SamplerState, DepthSampler, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(LENSFLARE_ORG_RESOURCES(ORG_HANDLE))
LENSFLARE_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/FlareOcclusion.hlsli"
#include "Common/FrameBuffer.hlsli"

// The lens flare visibility pass: one texel per light, how much of the light the depth leaves visible. ISLensFlare.hlsl draws the
// sprites that read it.

struct VS_INPUT
{
	float4 Position: POSITION0;
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
};

#if defined(VSHADER)
VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;
	vsout.Position = float4(input.Position.xy, 0.0, 1.0);
	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Visibility: SV_Target0;
};

#if defined(PSHADER)
static const uint MaxLights = 16;

#	if !defined(CS_ORG_BINDLESS)
SamplerState DepthSampler : register(s0);
Texture2D<float4> DepthTex : register(t0);

cbuffer PerGeometry : register(b2)
{
	float4 ScreenSpaceLightPos[MaxLights];  // xy: screen UV, z: depth a tap must reach to count as unoccluded
};
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	// The target is one texel per light
#	if defined(CS_ORG_BINDLESS)
	float4 light = OrgEngineRow(PerGeometryAt, min(uint(input.Position.x), MaxLights - 1));
#	else
	float4 light = ScreenSpaceLightPos[uint(input.Position.x)];
#	endif
	uint visibleSamples = 0;
	[unroll] for (uint i = 0; i < FlareOcclusion::SampleCount; i++)
	{
		// Off-screen taps count as visible, matching the sun glare
		float2 sampleUV = light.xy + FlareOcclusion::GetSampleOffset(i);
		visibleSamples += FrameBuffer::IsOutsideFrame(sampleUV) ||
		                  DepthTex.Sample(DepthSampler, FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(sampleUV)).x >= light.z;
	}
	psout.Visibility = FlareOcclusion::GetVisibility(visibleSamples);
	return psout;
}
#endif

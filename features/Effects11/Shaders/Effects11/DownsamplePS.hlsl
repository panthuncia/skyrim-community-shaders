#include "Common/OrgResources.hlsli"

// D3D11: t0 and s0. ORG: handles in the push block (Common/OrgResources.hlsli).
#define DOWNSAMPLE_RESOURCES(X)                 \
	X(SRV, Texture2D<float4>, SourceTexture, 0) \
	X(SAMPLER, SamplerState, LinearSampler, 0)
ORG_PUSH_BLOCK(DOWNSAMPLE_RESOURCES(ORG_HANDLE))
DOWNSAMPLE_RESOURCES(ORG_RESOURCE)

struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_Target
{
	float4 color = SourceTexture.SampleLevel(LinearSampler, input.txcoord0.xy, 0);
	bool4 finite = isfinite(color);
	float4 clamped = clamp(color, 0.0, 64512.0);
	// Per component: HLSL 2021 (DXC) has no vector ?:, and FXC has no select().
	return float4(finite.x ? clamped.x : 0.0, finite.y ? clamped.y : 0.0, finite.z ? clamped.z : 0.0, finite.w ? clamped.w : 0.0);
}

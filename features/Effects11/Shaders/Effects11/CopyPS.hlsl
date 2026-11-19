#include "Common/OrgResources.hlsli"

struct DitherConstants
{
	uint FrameCount;
	float DitherAmplitude;
	uint2 Pad;
};

// D3D11: t0 and b0. ORG: handles in the push block (Common/OrgResources.hlsli), the values a latch region.
#define COPY_RESOURCES(X)               \
	X(SRV, Texture2D, SourceTexture, 0) \
	X(LATCH, DitherConstants, Dither, 0)
ORG_PUSH_BLOCK(COPY_RESOURCES(ORG_HANDLE))
COPY_RESOURCES(ORG_RESOURCE)

struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

uint3 pcg3d(uint3 v)
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	return v;
}

float3 TriDither(float2 screenPos, uint frameCount)
{
	uint3 seed1 = uint3(screenPos, frameCount);
	uint3 seed2 = uint3(screenPos, frameCount + 4729u);
	return (float3(pcg3d(seed1)) - float3(pcg3d(seed2))) / float(0xFFFFFFFFu);
}

float4 main(PS_INPUT input) : SV_TARGET
{
	float3 color = SourceTexture.Load(int3(input.pos.xy, 0)).rgb;
	color += TriDither(input.pos.xy, Dither.FrameCount) * Dither.DitherAmplitude;
	return float4(color, 1.0);
}

#include "Upscaling/UpscaleVS.hlsl"

#if defined(PSHADER)
#	define ORG_SHARED_CONSTANTS
#	include "Common/OrgResources.hlsli"

// Must match Upscaling::JitterCB.
struct JitterConstants
{
	float2 jitter;
	float useWideKernel;
	float pad0;
};

// Jitter: D3D11 updates the constant buffer every frame; ORG reads it from the execution's latch
// region (Upscaling/ORGUpscaling.cpp).
#	define DEPTH_REFRACTION_UPSCALE_RESOURCES(X)   		X(SHARED, ConstantBuffer, Shared, 0)            		X(SRV, Texture2D<float4>, RefractionNormals, 0) 		X(SRV, Texture2D<float>, DepthTex, 1)           		X(SAMPLER, SamplerState, LinearSampler, 0)      		X(LATCH, JitterConstants, Jitter, 0)
ORG_PUSH_BLOCK(DEPTH_REFRACTION_UPSCALE_RESOURCES(ORG_HANDLE))

#	include "Common/FrameBuffer.hlsli"
#	include "Common/SharedData.hlsli"

DEPTH_REFRACTION_UPSCALE_RESOURCES(ORG_RESOURCE)

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 RefractionNormals: SV_TARGET0;
	float SAOCameraZ: SV_TARGET1;
	float Depth: SV_Depth;
};

float SampleMinDepth2x2(float2 uv)
{
	float4 depthQuad = DepthTex.GatherRed(LinearSampler, uv);
	return min(min(depthQuad.x, depthQuad.y), min(depthQuad.z, depthQuad.w));
}

float Min4(float4 v)
{
	return min(min(v.x, v.y), min(v.z, v.w));
}

float SampleMinDepthWideGather(float2 uv)
{
	// Gather-only wide footprint (4 offset 2x2 GatherRed calls) to save performance.
	float d0 = Min4(DepthTex.GatherRed(LinearSampler, uv, int2(-1, -1)));
	float d1 = Min4(DepthTex.GatherRed(LinearSampler, uv, int2(1, -1)));
	float d2 = Min4(DepthTex.GatherRed(LinearSampler, uv, int2(-1, 1)));
	float d3 = Min4(DepthTex.GatherRed(LinearSampler, uv, int2(1, 1)));
	return min(min(d0, d1), min(d2, d3));
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 originalUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);

	// Remove jitter offset to get the correct sampling coordinates
	float2 uv = originalUV - (Jitter.jitter * SharedData::BufferDim.zw);

	// Clamp within dynamic-resolution bounds.
	uv = FrameBuffer::ClampDynamicResolutionAdjustedScreenPosition(uv, input.TexCoord);

	// Upscale using linear sampling
	psout.RefractionNormals = RefractionNormals.SampleLevel(LinearSampler, uv, 0);
	psout.Depth = DepthTex.SampleLevel(LinearSampler, uv, 0);

	psout.SAOCameraZ = psout.Depth;

	return psout;
}

#endif

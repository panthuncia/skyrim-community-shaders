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
#	define UNDERWATER_MASK_UPSCALE_RESOURCES(X)  		X(SHARED, ConstantBuffer, Shared, 0)         		X(SRV, Texture2D<float>, UnderwaterMask, 0)  		X(SAMPLER, SamplerState, LinearSampler, 0)   		X(LATCH, JitterConstants, Jitter, 0)
ORG_PUSH_BLOCK(UNDERWATER_MASK_UPSCALE_RESOURCES(ORG_HANDLE))

#	include "Common/FrameBuffer.hlsli"
#	include "Common/Math.hlsli"
#	include "Common/SharedData.hlsli"

UNDERWATER_MASK_UPSCALE_RESOURCES(ORG_RESOURCE)

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float UnderwaterMask: SV_TARGET;
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 originalUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);

	// Remove jitter offset to get the correct sampling coordinates
	float2 uv = originalUV - (Jitter.jitter * SharedData::BufferDim.zw);

	// Clamp within bounds
	uv = clamp(uv, 0.0, FrameBuffer::DynamicResolutionParams1.xy);

	// Upscale using linear sampling with jitter-corrected coordinates
	psout.UnderwaterMask = UnderwaterMask.SampleLevel(LinearSampler, uv, 0);

	return psout;
}

#endif

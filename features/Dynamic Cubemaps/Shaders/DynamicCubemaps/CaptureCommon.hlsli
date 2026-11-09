#ifndef DYNAMIC_CUBEMAPS_CAPTURE_COMMON_HLSLI
#define DYNAMIC_CUBEMAPS_CAPTURE_COMMON_HLSLI

// Dual-target (Common/OrgResources.hlsli). Under ORG the shared constant buffers are heap reads
// (ORG_SHARED_CONSTANTS) and the capture constants are the frame's latch region, declared below.
#define ORG_SHARED_CONSTANTS
#include "Common/OrgResources.hlsli"

struct CaptureLightingState
{
	float ReferenceLuminance;
	uint PendingResetMask;
	uint Reset;
	uint Initialized;
};

struct UpdateCubemapConstants
{
	float3 CameraPreviousPosAdjust2;
	uint CaptureIndex;
	float CaptureDeltaTime;
	uint ResetCapture;
	uint2 UpdatePadding;
};

#define CAPTURE_RESOURCES(X)                                            \
	X(SHARED, ConstantBuffer, Shared, 0)                                \
	X(UAV, RWTexture2DArray<float4>, DynamicCubemap, 0)                 \
	X(UAV, RWTexture2DArray<float4>, DynamicCubemapRaw, 1)              \
	X(UAV, RWTexture2DArray<float4>, DynamicCubemapPosition, 2)         \
	X(UAV, RWStructuredBuffer<CaptureLightingState>, LightingState, 3) \
	X(SRV, Texture2D<float>, DepthTexture, 0)                           \
	X(SRV, Texture2D<float4>, ColorTexture, 1)                          \
	X(SAMPLER, SamplerState, LinearSampler, 0)                          \
	X(EXTERN_LATCH, UpdateCubemapConstants, UpdateData, 0)
ORG_PUSH_BLOCK(CAPTURE_RESOURCES(ORG_HANDLE))

#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "DynamicCubemaps/CubemapCommon.hlsli"

CAPTURE_RESOURCES(ORG_RESOURCE)

// The capture constants. D3D11: the cbuffer at b0. ORG: the frame's latch region, read through
// the names the cbuffer gives them.
#if defined(CS_ORG_BINDLESS)
ORG_RESOURCE(LATCH, UpdateCubemapConstants, UpdateData, 0)
#	define CameraPreviousPosAdjust2 UpdateData.CameraPreviousPosAdjust2
#	define CaptureIndex UpdateData.CaptureIndex
#	define CaptureDeltaTime UpdateData.CaptureDeltaTime
#	define ResetCapture UpdateData.ResetCapture
#else
cbuffer UpdateData : register(b0)
{
	float3 CameraPreviousPosAdjust2;
	uint CaptureIndex;
	float CaptureDeltaTime;
	uint ResetCapture;
	uint2 UpdatePadding;
}
#endif

static const float CaptureHistoryLifetime = 30.0;

float GetCaptureTime()
{
	return floor(fmod(SharedData::Timer, 64.0) * 16.0) / 16.0;
}

bool CaptureHistoryExpired(float lastSeen)
{
	return CaptureDeltaTime >= CaptureHistoryLifetime || fmod(GetCaptureTime() - lastSeen + 64.0, 64.0) >= CaptureHistoryLifetime;
}

float3 GetSamplingVector(uint3 texel)
{
	uint width, height, faces;
	DynamicCubemap.GetDimensions(width, height, faces);
	return GetCubemapSamplingVector(texel, float2(width, height));
}

bool SampleCapture(uint3 texel, out float3 position, out float3 color, out float2 uv)
{
	position = 0.0;
	color = 0.0;
	float3 viewDirection = FrameBuffer::WorldToView(-GetSamplingVector(texel), false);
	uv = FrameBuffer::ViewToUV(viewDirection, false);
	if (viewDirection.z >= 0.0 || !all(isfinite(uv)) || FrameBuffer::IsOutsideFrame(uv))
		return false;

	float2 sampleUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
#if defined(REFLECTIONS)
	bool excludeSky = false;
#else
	bool excludeSky = true;
#endif
	if (excludeSky) {
		float4 depths = DepthTexture.GatherRed(LinearSampler, sampleUV);
		if (any(depths >= 1.0))
			return false;
	}
	float depth = DepthTexture.SampleLevel(LinearSampler, sampleUV, 0);
	if (SharedData::GetScreenDepth(depth) <= 16.5)
		return false;

	float4 positionCS = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0));
	position = positionCS.xyz / positionCS.w * 0.001;
	color = ColorManagement::SceneToLinear(ColorTexture.SampleLevel(LinearSampler, sampleUV, 0).rgb);
	if (!all(isfinite(position)) || !all(isfinite(color)))
		return false;
	color = clamp(color, 0.0, 65504.0);
	return true;
}

float3 AdjustCapturePosition(float3 position)
{
	return position + (CameraPreviousPosAdjust2 - FrameBuffer::CameraPosAdjust.xyz) * 0.001;
}

bool CaptureGeometryMatches(float3 previousPosition, float3 position)
{
	return length(previousPosition - position) <= max(0.032, length(position) * 0.03);
}

#endif

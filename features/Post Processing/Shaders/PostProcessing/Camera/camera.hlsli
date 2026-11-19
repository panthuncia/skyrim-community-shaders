// Dual-target (Common/OrgResources.hlsli), for camera.ps.hlsl and camera_finish.ps.hlsl. D3D11 (Camera::Draw) binds the
// registers below and reads CameraCB (Camera::CameraCB) at b1, declared as before. Under ORG (ORG/ORGCamera.cpp) the
// same fields are the LATCH entry CameraFrame, each copied into a static of the same name (ORG_GLOBAL_LATCH_BEGIN).
// Film grain reads SharedData's frame count, so both passes list the shared constants, and this header comes before
// any header that declares them.
#define ORG_SHARED_CONSTANTS
#include "Common/OrgResources.hlsli"
#include "PostProcessing/fullscreen.hlsli"

#define CAMERA_CB_FIELDS(M)     \
	M(float4, CameraScreen)     \
	M(float4, CameraLens)       \
	M(float4, CameraAberration) \
	M(float4, CameraSoftness)   \
	M(float4, CameraGrain)

#define CAMERA_RESOURCES(X)                    \
	X(SHARED, ConstantBuffer, Shared, 0)       \
	X(SRV, Texture2D<float4>, InputTexture, 0) \
	X(SAMPLER, SamplerState, ColorSampler, 0)
ORG_PUSH_BLOCK(CAMERA_RESOURCES(ORG_HANDLE) ORG_HANDLE(LATCH, CameraConstants, CameraFrame, 1))
CAMERA_RESOURCES(ORG_RESOURCE)

ORG_GLOBAL_LATCH_BEGIN(CameraConstants, CameraCB, 1)
CAMERA_CB_FIELDS(ORG_GLOBAL_MEMBER)
ORG_GLOBAL_LATCH_END(CameraConstants, CameraFrame, 1)
#define ORG_GLOBAL_LATCH CameraFrame
CAMERA_CB_FIELDS(ORG_GLOBAL_STATIC)
#undef ORG_GLOBAL_LATCH

float2 CameraAspectScale()
{
	return float2(CameraLens.y, 1.0);
}

float CameraNormalizedRadius(float2 uv)
{
	return length((uv - 0.5) * CameraAspectScale()) / CameraLens.z;
}

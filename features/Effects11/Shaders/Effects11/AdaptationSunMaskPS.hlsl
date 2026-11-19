#include "Common/OrgResources.hlsli"

// The matrices are rows, built into float4x4 where they are used: a latch is loaded with Load<T>, which does not keep
// row_major (Common/OrgResources.hlsli, LATCH).
struct SunMaskConstants
{
	float4 CameraViewProj[4];
	float4 CameraViewProjInverse[4];
	float4 SunDirection;
	float4 DynamicResolution;
	float4 MaskParams;
};

// D3D11: t0, s0 and b0. ORG: handles in the push block (Common/OrgResources.hlsli), the values a latch region.
#define SUN_MASK_RESOURCES(X)                   \
	X(SRV, Texture2D<float4>, SourceTexture, 0) \
	X(SAMPLER, SamplerState, LinearSampler, 0)  \
	X(LATCH, SunMaskConstants, SunMask, 0)
ORG_PUSH_BLOCK(SUN_MASK_RESOURCES(ORG_HANDLE))
SUN_MASK_RESOURCES(ORG_RESOURCE)

struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_Target
{
	float2 uv = input.txcoord0;
	float4 color = SourceTexture.SampleLevel(LinearSampler, uv, 0);

	float2 ndc = float2(uv.x * SunMask.DynamicResolution.z, 1.0 - uv.y * SunMask.DynamicResolution.w) * 2.0 - 1.0;
	float4 positionMS = mul(float4x4(SunMask.CameraViewProjInverse[0], SunMask.CameraViewProjInverse[1], SunMask.CameraViewProjInverse[2], SunMask.CameraViewProjInverse[3]), float4(ndc, 0.5, 1.0));
	float3 viewDirection = normalize(positionMS.xyz / positionMS.w);

	float cosTheta = dot(viewDirection, SunMask.SunDirection.xyz);
	if (cosTheta <= SunMask.MaskParams.x)
		return color;

	float3 tangent = viewDirection - SunMask.SunDirection.xyz * cosTheta;
	float tangentLength = length(tangent);
	float3 fallbackAxis = abs(SunMask.SunDirection.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
	tangent = tangentLength > 1e-5 ? tangent / tangentLength : normalize(cross(SunMask.SunDirection.xyz, fallbackAxis));
	float3 sampleDirection = SunMask.SunDirection.xyz * SunMask.MaskParams.y + tangent * SunMask.MaskParams.z;

	float4 sampleCS = mul(float4x4(SunMask.CameraViewProj[0], SunMask.CameraViewProj[1], SunMask.CameraViewProj[2], SunMask.CameraViewProj[3]), float4(sampleDirection, 0.0));
	if (sampleCS.w <= 1e-5)
		return color;

	float2 sampleNDC = sampleCS.xy / sampleCS.w;
	float2 sampleUV = float2(sampleNDC.x * 0.5 + 0.5, 0.5 - sampleNDC.y * 0.5) * SunMask.DynamicResolution.xy;
	return SourceTexture.SampleLevel(LinearSampler, saturate(sampleUV), 0);
}

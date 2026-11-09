// ENB [RAYS] screen-space shafts from the sun or a moon.
// MASK draws the light's sky disc at half resolution, BLUR smears it radially towards the light,
// and COMPOSITE adds the result to the main target.

// Dual-target (Common/OrgResources.hlsli). Under ORG the shared constant buffers and the included headers'
// resources (SharedData's depth, Cloud Shadows) are heap reads through the push block (ORG_SHARED_CONSTANTS,
// ORG_HEADER_RESOURCES). SunRaysData changes every frame and between the passes: under ORG each pass reads its
// own region of the frame's latch.
#define ORG_SHARED_CONSTANTS
#define ORG_HEADER_RESOURCES
#include "Common/OrgResources.hlsli"

struct SunRaysData
{
	float2 LightUV;
	float2 UVScale;
	float2 UVMax;
	float BlurFactor;
	uint StepCount;
	float3 LightDirection;
	float LightBillboardTan;
	float3 RaysColor;
	float MaskBrightness;
	float MaskExponent;
	uint WeightedSteps;
	uint UseNoise;
	float InvScreenWidth;
};

#define SUN_RAYS_RESOURCES(X)                           \
	X(SHARED, ConstantBuffer, Shared, 0)                   \
	X(SAMPLER, SamplerState, LinearSampler, 0)             \
	X(SRV, Texture2D<float>, RaysTexture, 0)               \
	X(LATCH, SunRaysData, Rays, 1)                         \
	X(EXTERN, Texture2D<float4>, DepthTexture, 17)         \
	X(EXTERN, TextureCube<float>, CloudShadowsTexture, 25)
ORG_PUSH_BLOCK(SUN_RAYS_RESOURCES(ORG_HANDLE))

#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

SUN_RAYS_RESOURCES(ORG_RESOURCE)

#if defined(MASK) && defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

struct VS_OUTPUT_POST
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

static const float SkyDepth = 0.999997;

float SampleRays(float2 uv)
{
	return RaysTexture.SampleLevel(LinearSampler, min(saturate(uv) * Rays.UVScale, Rays.UVMax), 0);
}

float RadialBlur(float2 uv, float2 target, float noise)
{
	float stepInv = rcp(float(Rays.StepCount));
	float shift = -stepInv * noise;
	float rays = 0.0;
	[loop] for (uint i = 0; i < Rays.StepCount; i++)
	{
		shift += stepInv;
		float weight = Rays.WeightedSteps ? 1.0 - shift : 1.0;
		rays += SampleRays(lerp(uv, target, shift)) * weight;
	}
	return rays * stepInv;
}

float GetNoise(float2 pixel)
{
	return Rays.UseNoise ? Random::InterleavedGradientNoise(pixel, SharedData::FrameCount) : 0.5;
}

#if defined(MASK)
float main(VS_OUTPUT_POST input) : SV_Target0
{
	float2 uv = input.txcoord0;
	if (SharedData::GetDepth(uv) < SkyDepth)
		return 0.0;

	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, 0.5, 1);
	float4 positionMS = mul(FrameBuffer::CameraViewProjInverse, positionCS);
	float3 viewDirection = normalize(positionMS.xyz / positionMS.w);

	float cosAngle = dot(viewDirection, Rays.LightDirection);
	if (cosAngle <= 0.0)
		return 0.0;

	float tanAngleSq = max(1.0 - cosAngle * cosAngle, 0.0) / (cosAngle * cosAngle);
	float radiusSq = tanAngleSq / max(Rays.LightBillboardTan * Rays.LightBillboardTan, 1e-8);
	float mask = Rays.MaskBrightness * pow(saturate(1.0 - radiusSq), Rays.MaskExponent);

#	if defined(CLOUD_SHADOWS)
	mask *= saturate(1.0 - CloudShadows::CloudShadowsTexture.SampleLevel(LinearSampler, viewDirection, 0));
#	endif

	float2 edge = abs(uv * 2.0 - 1.0);
	float vignette = max(edge.x, edge.y);
	return mask * (1.0 - vignette * vignette);
}
#elif defined(BLUR)
float main(VS_OUTPUT_POST input) : SV_Target0
{
	float2 uv = input.txcoord0;
	return RadialBlur(uv, lerp(uv, Rays.LightUV, Rays.BlurFactor), GetNoise(input.pos.xy));
}
#elif defined(COMPOSITE)
float4 main(VS_OUTPUT_POST input) : SV_Target0
{
	float2 uv = input.txcoord0;
	float3 direction = normalize(float3(Rays.LightUV - uv, 0.035));
	float2 target = uv + direction.xy * (Rays.InvScreenWidth * float(Rays.StepCount) * 6.0);
	float rays = RadialBlur(uv, target, GetNoise(input.pos.xy));
	return float4(rays * ColorManagement::SRGBToWorking(Rays.RaysColor), 1.0);
}
#endif

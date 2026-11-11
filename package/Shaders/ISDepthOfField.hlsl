#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0-t4 and s0-s4 from the heap, the shared
// constants, and PerGeometry from the engine-constants capture through the pass's latch.
// Depth of field and the distant blur (DISTANT) share this source.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISDEPTHOFFIELD_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, ImageTex, 0) X(SRV, Texture2D<float4>, BlurredTex, 1) X(SRV, Texture2D<float>, DepthTex, 2) X(SRV, Texture2D<float4>, AvgDepthTex, 3) X(SRV, Texture2D<float4>, MaskTex, 4) X(SAMPLER, SamplerState, ImageSampler, 0) X(SAMPLER, SamplerState, BlurredSampler, 1) X(SAMPLER, SamplerState, DepthSampler, 2) X(SAMPLER, SamplerState, AvgDepthSampler, 3) X(SAMPLER, SamplerState, MaskSampler, 4) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISDEPTHOFFIELD_ORG_RESOURCES(ORG_HANDLE))
ISDEPTHOFFIELD_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
static float4 invScreenRes = OrgEngineRow(PerGeometryAt, 0);  // inverse render target width and height in xy
static float4 params = OrgEngineRow(PerGeometryAt, 1);        // DOF near range in x, far range in y
static float4 params2 = OrgEngineRow(PerGeometryAt, 2);       // DOF near blur in x, far blur in w
static float4 params3 = OrgEngineRow(PerGeometryAt, 3);       // 1 / (far - near) in z, near / (far - near) in w
static float4 params4 = OrgEngineRow(PerGeometryAt, 4);
static float4 params5 = OrgEngineRow(PerGeometryAt, 5);
static float4 params6 = OrgEngineRow(PerGeometryAt, 6);
static float4 params7 = OrgEngineRow(PerGeometryAt, 7);
#	else
SamplerState ImageSampler : register(s0);
SamplerState BlurredSampler : register(s1);
SamplerState DepthSampler : register(s2);
SamplerState AvgDepthSampler : register(s3);
SamplerState MaskSampler : register(s4);

Texture2D<float4> ImageTex : register(t0);
Texture2D<float4> BlurredTex : register(t1);
Texture2D<float> DepthTex : register(t2);
Texture2D<float4> AvgDepthTex : register(t3);
Texture2D<float4> MaskTex : register(t4);

cbuffer PerGeometry : register(b2)
{
	float4 invScreenRes : packoffset(c0);  // inverse render target width and height in xy
	float4 params : packoffset(c1);        // DOF near range in x, far range in y
	float4 params2 : packoffset(c2);       // DOF near blur in x, far blur in w
	float4 params3 : packoffset(c3);       // 1 / (far - near) in z, near / (far - near) in w
	float4 params4 : packoffset(c4);
	float4 params5 : packoffset(c5);
	float4 params6 : packoffset(c6);
	float4 params7 : packoffset(c7);
};
#	endif

void CheckOffsetDepth(float2 center, float2 offset, inout float crossSection,
	inout float totalDepth)
{
	float depth = DepthTex.Sample(DepthSampler, FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(invScreenRes.xy * offset + center));

	float crossSectionDelta = 0;
	if (depth > 0.999998987) {
		crossSectionDelta = (1. / 9.);
	}
	crossSection += crossSectionDelta;
	totalDepth += depth;
}

float GetFinalDepth(float depth, float near, float far)
{
	return (2 * near * far) / ((far + near) - (depth * 2 - 1) * (far - near));
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 adjustedTexCoord = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);
	float2 imageTexCoord = invScreenRes.z > 0.5 ? adjustedTexCoord : input.TexCoord;

	float3 imageColor = ImageTex.Sample(ImageSampler, imageTexCoord).xyz;
	float3 blurColor = BlurredTex.Sample(BlurredSampler, imageTexCoord).xyz;

	float mask = 1;
	float4 dofParams = params;
	float4 dofParams2 = params2;
#	if defined(MASKED)
	mask = MaskTex.Sample(MaskSampler, adjustedTexCoord).x;
	dofParams = lerp(params, params6, mask);
	dofParams2 = lerp(params2, params7, mask);
#	endif

	float2 dofBlurRange = float2(dofParams2.x, dofParams.x);
	float focusDistance = dofParams.y;

#	if !defined(MASKED)
	if (params3.z > 0) {
		focusDistance = AvgDepthTex.Sample(AvgDepthSampler, 0).x;
		float depthFactor = saturate(focusDistance * params3.z - params3.w);
		dofBlurRange = lerp(float2(params2.x, params.x), float2(params2.w, params.y), depthFactor);
	}
#	endif

	float depthCC = DepthTex.Sample(DepthSampler, adjustedTexCoord);

	float crossSection = 0;
	float avgDepth = depthCC;
	bool isTooDeep = false;
	if (dofParams2.w != 0 && depthCC > 0.999998987) {
		crossSection = 1. / 9.;
		float totalDepth = depthCC;
		CheckOffsetDepth(input.TexCoord, float2(-3, -3), crossSection, totalDepth);
		CheckOffsetDepth(input.TexCoord, float2(-3, 0), crossSection, totalDepth);
		CheckOffsetDepth(input.TexCoord, float2(-3, 3), crossSection, totalDepth);
		CheckOffsetDepth(input.TexCoord, float2(3, -3), crossSection, totalDepth);
		CheckOffsetDepth(input.TexCoord, float2(3, 0), crossSection, totalDepth);
		CheckOffsetDepth(input.TexCoord, float2(3, 3), crossSection, totalDepth);
		CheckOffsetDepth(input.TexCoord, float2(0, -3), crossSection, totalDepth);
		CheckOffsetDepth(input.TexCoord, float2(0, 3), crossSection, totalDepth);

		avgDepth = totalDepth / 9;
		isTooDeep = avgDepth > 0.999998987;
	}

	float blurFactor = 0;
	float finalDepth = avgDepth;
	if (!isTooDeep && avgDepth > 1e-5) {
		float depth, near, far;
		if (avgDepth <= 0.01) {
			depth = 100 * avgDepth;
			near = params3.x;
			far = params3.y;
		} else {
			depth = 1.01 * avgDepth - 0.01;
			near = dofParams.z;
			far = dofParams.w;
		}
		finalDepth = GetFinalDepth(depth, near, far);

		float dofStrength = 0;
#	if defined(DISTANT)
		dofStrength = (finalDepth - focusDistance) / dofBlurRange.y;
#	else
		if ((focusDistance > finalDepth || mask == 0) && dofParams2.y != 0) {
			dofStrength = (focusDistance - finalDepth) / dofBlurRange.y;
		} else if (finalDepth > focusDistance && dofParams2.z != 0) {
			dofStrength = (finalDepth - focusDistance) / dofBlurRange.y;
		}
#	endif

		blurFactor = saturate(dofStrength) * (dofBlurRange.x * (1 - 0.5 * crossSection));
	}

	float3 finalColor = lerp(imageColor, blurColor, blurFactor);
#	if defined(FOGGED)
	float fogFactor = (params4.w * saturate((finalDepth - params5.y) / (params5.x - params5.y))) * mask;
	finalColor = lerp(finalColor, params4.xyz, fogFactor);
#	endif

	psout.Color = float4(finalColor, 1);

	return psout;
}
#endif

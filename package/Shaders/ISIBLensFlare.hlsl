#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0 and s0 from the heap (the ramps and masks
// are declared but never read), the shared constants, and PerGeometry from the engine-constants capture through the pass's latch.
// The vertex stage is the graph's fullscreen triangle.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISIBLENSFLARE_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, DownScaledBlurredHDRTex, 0) X(SAMPLER, SamplerState, DownScaledBlurredHDRSampler, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISIBLENSFLARE_ORG_RESOURCES(ORG_HANDLE))
ISIBLENSFLARE_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
static float lensCount = OrgEngineRow(PerGeometryAt, 0).x;
static float flareDispersal = OrgEngineRow(PerGeometryAt, 0).y;
static float lightsRangeDownshift = OrgEngineRow(PerGeometryAt, 0).z;
static float invLightsRangeDownshift = OrgEngineRow(PerGeometryAt, 0).w;
static float lateralRepeat = OrgEngineRow(PerGeometryAt, 1).x;
static float channelsDistortionRed = OrgEngineRow(PerGeometryAt, 1).y;
static float channelsDistortionGreen = OrgEngineRow(PerGeometryAt, 1).z;
static float channelsDistortionBlue = OrgEngineRow(PerGeometryAt, 1).w;
static float texelOffsetX = OrgEngineRow(PerGeometryAt, 2).x;
static float texelOffsetY = OrgEngineRow(PerGeometryAt, 2).y;
static float haloFetch = OrgEngineRow(PerGeometryAt, 2).z;
static float haloWidthPow = OrgEngineRow(PerGeometryAt, 2).w;
static float dynamicSource = OrgEngineRow(PerGeometryAt, 3).x;
static float globalIntensity = OrgEngineRow(PerGeometryAt, 3).y;
#	else
SamplerState DownScaledBlurredHDRSampler : register(s0);
SamplerState RampsSampler : register(s1);
SamplerState MasksSampler : register(s2);

Texture2D<float4> DownScaledBlurredHDRTex : register(t0);
Texture2D<float4> RampsTex : register(t1);
Texture2D<float4> MasksTex : register(t2);

cbuffer PerGeometry : register(b2)
{
	float lensCount : packoffset(c0.x);
	float flareDispersal : packoffset(c0.y);
	float lightsRangeDownshift : packoffset(c0.z);
	float invLightsRangeDownshift : packoffset(c0.w);
	float lateralRepeat : packoffset(c1.x);
	float channelsDistortionRed : packoffset(c1.y);
	float channelsDistortionGreen : packoffset(c1.z);
	float channelsDistortionBlue : packoffset(c1.w);
	float texelOffsetX : packoffset(c2.x);
	float texelOffsetY : packoffset(c2.y);
	float haloFetch : packoffset(c2.z);
	float haloWidthPow : packoffset(c2.w);
	float dynamicSource : packoffset(c3.x);
	float globalIntensity : packoffset(c3.y);  // vanilla's place; the engine fills it by name
};
#	endif

float4 GetSampleColor(float2 texCoord)
{
	float4 color = DownScaledBlurredHDRTex.Sample(DownScaledBlurredHDRSampler, texCoord);
	// Vanilla clamps both the bright-pass test and the color to [0, 1].
	if (saturate(color.x - lightsRangeDownshift) + saturate(color.y - lightsRangeDownshift) +
			saturate(color.z - lightsRangeDownshift) <=
		0) {
		return 0;
	}
	return saturate(color);
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float4 lensColor = 0;
	for (int sampleIndex = -asint(lateralRepeat); sampleIndex <= asint(lateralRepeat); ++sampleIndex) {
		float4 sampleColor;
		if (dynamicSource > 0.5) {
			sampleColor = GetSampleColor(FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(
				input.TexCoord + float2(texelOffsetX * sampleIndex, 0)));
		} else {
			sampleColor = GetSampleColor(input.TexCoord + float2(texelOffsetX * sampleIndex, 0));
		}
		lensColor += sampleColor;
	}
	psout.Color = globalIntensity * lensColor;

	return psout;
}
#endif

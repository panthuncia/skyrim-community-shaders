#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0-t1 and s0-s1 from the heap, FrameBuffer
// from the shared constants, PerGeometry from the engine-constants capture through the pass's latch.
#	define ORG_SHARED_CONSTANTS
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISBLUR_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, ImageTex, 0) X(SRV, Texture2D<float4>, AvgLumTex, 1) X(SAMPLER, SamplerState, ImageSampler, 0) X(SAMPLER, SamplerState, AvgLumSampler, 1) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISBLUR_ORG_RESOURCES(ORG_HANDLE))
ISBLUR_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/Color.hlsli"
#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
// BlurRadius has a row of its own in the cbuffer: BlurOffsets starts at c3.
struct ISBlurPerGeometry
{
	float4 BlurBrightPass;
	float4 BlurScale;
	float BlurRadius;
	float3 OrgPadding;
	float4 BlurOffsets[16];
};
ORG_ENGINE_CONSTANTS(ISBlurPerGeometry, PerGeometry, PerGeometryAt)
static float4 BlurBrightPass = PerGeometry.BlurBrightPass;
static float4 BlurScale = PerGeometry.BlurScale;
static float BlurRadius = PerGeometry.BlurRadius;
static float4 BlurOffsets[16] = PerGeometry.BlurOffsets;
#	else
SamplerState ImageSampler : register(s0);
SamplerState AvgLumSampler : register(s1);

Texture2D<float4> ImageTex : register(t0);
Texture2D<float4> AvgLumTex : register(t1);

cbuffer PerGeometry : register(b2)
{
	float4 BlurBrightPass : packoffset(c0);
	float4 BlurScale : packoffset(c1);
	float BlurRadius : packoffset(c2);
	float4 BlurOffsets[16] : packoffset(c3);
};
#	endif

float4 GetImageColor(float2 texCoord, float blurScale)
{
	return ImageTex.Sample(ImageSampler, texCoord) * float4(blurScale.xxx, 1);
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float4 color = 0;

#	if defined(TEXTAP)
	int blurRadius = TEXTAP;
#	else
	uint blurRadius = asuint(BlurRadius);
#	endif
	float2 blurScale = BlurScale.zw;
#	if !defined(TEXTAP) || !defined(COLORRANGE)
	blurScale = 1;
#	endif

	for (int blurIndex = 0; blurIndex < blurRadius; ++blurIndex) {
		float2 screenPosition = BlurOffsets[blurIndex].xy + input.TexCoord.xy;
		float4 imageColor = 0;
		[branch] if (BlurScale.x < 0.5)
		{
			imageColor = GetImageColor(FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(screenPosition), blurScale.y);
		}
		else
		{
			imageColor = GetImageColor(screenPosition, blurScale.y);
		}
#	if defined(BRIGHTPASS)
		imageColor = BlurBrightPass.y * max(0, -BlurBrightPass.x + imageColor);
#	endif
		color += imageColor * BlurOffsets[blurIndex].z;
	}

#	if defined(BRIGHTPASS)
	// Vanilla scales the average luminance texel by the color range, like the image taps.
	float avgLum = Color::RGBToLuminance(AvgLumTex.Sample(AvgLumSampler, input.TexCoord.xy).xyz * blurScale.y);
	color.w = avgLum;
#	endif

	psout.Color = color * float4(blurScale.xxx, 1);

	return psout;
}
#endif

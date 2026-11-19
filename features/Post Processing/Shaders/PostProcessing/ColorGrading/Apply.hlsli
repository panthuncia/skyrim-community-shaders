#ifndef POST_PROCESSING_COLOR_GRADING_APPLY_HLSLI
#define POST_PROCESSING_COLOR_GRADING_APPLY_HLSLI

#include "Common/Color.hlsli"
#include "PostProcessing/common.hlsli"

#include "Common/OrgResources.hlsli"

// The same constants and LUT are used by the standalone and fused pixel shaders.
// The fused one (Composite) is D3D11 only. The standalone one (colorgrading.hlsl) declares TexLUT and
// GradingSampler in its own resource list, with its push block, before it includes this file.
#ifdef COMPOSITE_APPLY_LUT
#	if defined(CS_ORG_BINDLESS)
#		error "the fused grading is D3D11 only: ORG runs Composite and ColorGrading as separate passes"
#	endif
Texture3D<float4> TexLUT : register(t15);
SamplerState GradingSampler : register(s2);
#endif

#define LUT_SIZE 64

#include "PostProcessing/ColorGrading/Include/OpenDRTConfig.hlsli"

// ColorGrading::ColorCB. D3D11: cbuffer ColorCB, at b0 in the fused shader and b1 in the standalone one. ORG: the
// fields of the ColorGradingFrame latch, read through statics of the same names (Common/OrgResources.hlsli).
#define COLOR_CB_FIELDS(M, A)                                                                    	A(float4, asccdl, 3)                                                                         	A(float4, liftgammagain, 3) /* lift，gamma，gain */                                          	M(float4, inOutGamma)       /* .z = input gamma, .w = output gamma */                        	M(float4, oklchSaturation)                                                                   	A(float4, oklchColorMixer, 7)                                                                	M(float4, contrast)                                                                          	M(float4, pivot)                                                                             	M(float4, exposureTemperatureTint)                                                           	M(float4, shadows)                                                                           	M(float4, midtones)                                                                          	M(float4, highlights)                                                                        	M(float4, shadowsHighlightsRange) /* shadowBegin, shadowEnd, highlightBegin, highlightEnd */ 	A(float4, tonemapParams, 2)                                                                  	A(float4, inputToWorking, 3)   /* scene RGB → working color space */                         	A(float4, workingToTonemap, 3) /* working → tonemapper native space */                       	A(float4, tonemapToOutput, 3)  /* tonemapper native → output space */                        	A(float4, workingToXYZ, 3)     /* working → XYZ D65 (white balance and Oklab) */             	A(float4, xyzToWorking, 3)     /* XYZ D65 → working */                                       	M(float4, workingWhitePoint)   /* .xy = D65 reference white of the XYZ matrices */           	M(float4, shadowsOffset)       /* SMH color offsets */                                       	M(float4, midtonesOffset)                                                                    	M(float4, highlightsOffset)                                                                  	M(float4, cinematic) /* game value: saturation, brightness, contrast */                      	M(float4, fade)      /* color */                                                             	M(float4, tint)      /* color */                                                             	M(uint, logType)                                                                             	M(uint, skipLDR)                                                                             	M(uint, skipLUT)                                                                             	M(uint, enableTonemap)                                                                       	M(uint, enableColorSpaceTransform)                                                           	M(uint, enableHDR)          /* HDR display is enabled (auto-set from HDR feature) */         	M(float, hdrPeakNits)       /* Maximum display brightness in nits for HDR */                 	M(float, hdrPaperWhiteNits) /* Reference white brightness in nits for HDR */                 	M(OpenDRTConfig, odrtConfig)

#ifdef COMPOSITE_APPLY_LUT
ORG_GLOBAL_LATCH_BEGIN(ColorConstants, ColorCB, 0)
#else
ORG_GLOBAL_LATCH_BEGIN(ColorConstants, ColorCB, 1)
#endif
COLOR_CB_FIELDS(ORG_GLOBAL_MEMBER, ORG_GLOBAL_ARRAY)
ORG_GLOBAL_LATCH_END(ColorConstants, ColorGradingFrame, 1)
#define ORG_GLOBAL_LATCH ColorGradingFrame
COLOR_CB_FIELDS(ORG_GLOBAL_STATIC, ORG_GLOBAL_STATIC_ARRAY)
#undef ORG_GLOBAL_LATCH

float3 LogToLinear(float3 logColor)
{
	const float linearRange = 14.0f;
	const float linearGrey = 0.18f;
	const float exposureGrey = 444.0f;
	return exp2((logColor - exposureGrey / 1023.0) * linearRange) * linearGrey;
}

float3 LinearToLog(float3 linearColor)
{
	const float linearRange = 14.0f;
	const float linearGrey = 0.18f;
	const float exposureGrey = 444.0f;
	return saturate(log2(linearColor) / linearRange - log2(linearGrey) / linearRange + exposureGrey / 1023.0f);
}

float3 ApplyLUT(float3 color)
{
	color = LinearToLog(color + LogToLinear(0));
	color = color * ((LUT_SIZE - 1.0) / LUT_SIZE) + (0.5 / LUT_SIZE);
	color = TexLUT.SampleLevel(GradingSampler, color, 0).xyz;
	return color;
}

float3 ApplyGameCinematic(float3 linearColor)
{
	// Game cinematic
	float3 color = pow(abs(linearColor), inOutGamma.z) * cinematic.y;
	color = Saturation(color, cinematic.x);
	color = LinearContrast(color, cinematic.z, 0.18);
	return color;
}

float3 ApplyGameTintFade(float3 color)
{
	// Game tint/fade colors are Rec.709, while HDR grading output is Rec.2020.
	float luma = dot(color, enableHDR ? Rec2020_2_XYZ_MAT[1] : sRGB_2_XYZ_MAT[1]);
	float3 outputTint = enableHDR ? Color::BT709ToBT2020(tint.xyz) : tint.xyz;
	color = lerp(color, luma * outputTint, tint.w);

	// Game fade
	float3 outputFade = enableHDR ? Color::BT709ToBT2020(fade.xyz) : fade.xyz;
	return lerp(color, outputFade, fade.w);
}

float4 ApplyGradeOutput(float3 color)
{
	return float4(ApplyGameTintFade(pow(abs(color), inOutGamma.w)), 1);
}

#endif

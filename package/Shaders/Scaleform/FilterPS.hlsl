// Scaleform GFx 3.3's filter pixel shaders (GRendererD3D1x, the exe's PS-B table), transcribed from the HLSL source the exe
// embeds (docs/development/p5/scaleform.md, "Shaders"): blurs, drop shadows, glows and bevels (DrawBlurRect) and the colour
// matrix (DrawColorMatrixRect). FXC builds each variant as the game's own: the same constants, resources and instructions as
// the embedded source compiled by FXC.
//
// FILTER, one per distinct embedded source (in the exe's order), and the PS-B slots that use it:
//    1 inner shadow (slot 1)             2 inner highlight (2, 6)          3 inner shadow knockout (5)
//    4-6: 1-3, multiply (3; 4, 8; 7)
//    7 outer shadow (9)                  8 outer highlight (10)            9 outer highlight knockout (14)
//   10 outer shadow knockout (13)        11-14: 7-10, multiply (11, 12, 16, 15)
//   15 shadow only (17, 18)              16 highlight only (no slot)       17-18: 15-16, multiply (19, 20; none)
//   19 one-pass blur (21)                20 the same, multiply (23)        21 two-dimensional blur (22)  22 multiply (24)
//   23 colour matrix (25)                24 colour matrix, multiply (26)
// "Highlight" sums the alpha at +offset into a and at -offset into r: a bevel's two sides.
//
// The ORG build (CS_ORG_BINDLESS) reads the constants from the staging buffer and the textures and samplers from the
// descriptor heaps (Scaleform/Filter.hlsli).

#ifndef FILTER
#	define FILTER 1
#endif

// What each variant is made of. Constants: A (source and filtered texture), B (filtered only, with a colour), C (blur), D
// (colour matrix). Taps: BOX (the colour at +offset), HIGHLIGHT (alpha at +offset and -offset), BLUR1 (a row), BLUR2 (a box
// without offset). Base: the source texture read with SampleLevel (1) or Sample (2), or not read (0).
#define CB_A 1
#define CB_B 2
#define CB_C 3
#define CB_D 4
#define TAPS_BOX 1
#define TAPS_HIGHLIGHT 2
#define TAPS_BLUR1 3
#define TAPS_BLUR2 4
#define TAPS_NONE 5
// Combines: INNER lerp(scolor, base, a) * base.a; INNER_HIGHLIGHT; INNER_KNOCKOUT scolor * (1 - a) * base.a; OUTER
// scolor * a * (1 - base.a) + base; OUTER_HIGHLIGHT; SHADOW scolor * a; BLUR the taps alone; MATRIX the colour matrix.
#define COMBINE_INNER 1
#define COMBINE_INNER_HIGHLIGHT 2
#define COMBINE_INNER_KNOCKOUT 3
#define COMBINE_OUTER 4
#define COMBINE_OUTER_HIGHLIGHT 5
#define COMBINE_SHADOW 6
#define COMBINE_BLUR 7
#define COMBINE_MATRIX 8

#if FILTER == 1 || FILTER == 4
#	define FILTER_CB CB_A
#	define FILTER_TAPS TAPS_BOX
#	define FILTER_BASE 1
#	define FILTER_COMBINE COMBINE_INNER
#elif FILTER == 2 || FILTER == 5
#	define FILTER_CB CB_A
#	define FILTER_TAPS TAPS_HIGHLIGHT
#	define FILTER_BASE 1
#	define FILTER_COMBINE COMBINE_INNER_HIGHLIGHT
#elif FILTER == 3 || FILTER == 6
#	define FILTER_CB CB_A
#	define FILTER_TAPS TAPS_BOX
#	define FILTER_BASE 1
#	define FILTER_COMBINE COMBINE_INNER_KNOCKOUT
#elif FILTER == 7 || FILTER == 10 || FILTER == 11 || FILTER == 14
#	define FILTER_CB CB_A
#	define FILTER_TAPS TAPS_BOX
#	define FILTER_BASE 2
#	define FILTER_COMBINE COMBINE_OUTER
#	define FILTER_KNOCKOUT (FILTER == 10 || FILTER == 14)
#elif FILTER == 8 || FILTER == 9 || FILTER == 12 || FILTER == 13
#	define FILTER_CB CB_A
#	define FILTER_TAPS TAPS_HIGHLIGHT
#	define FILTER_BASE 2
#	define FILTER_COMBINE COMBINE_OUTER_HIGHLIGHT
#	define FILTER_KNOCKOUT (FILTER == 9 || FILTER == 13)
#elif FILTER == 15 || FILTER == 17
#	define FILTER_CB CB_B
#	define FILTER_TAPS TAPS_BOX
#	define FILTER_BASE 0
#	define FILTER_COMBINE COMBINE_SHADOW
#elif FILTER == 16 || FILTER == 18
#	define FILTER_CB CB_B
#	define FILTER_TAPS TAPS_HIGHLIGHT
#	define FILTER_BASE 0
#	define FILTER_COMBINE COMBINE_SHADOW
#elif FILTER == 19 || FILTER == 20
#	define FILTER_CB CB_C
#	define FILTER_TAPS TAPS_BLUR1
#	define FILTER_BASE 0
#	define FILTER_COMBINE COMBINE_BLUR
#elif FILTER == 21 || FILTER == 22
#	define FILTER_CB CB_C
#	define FILTER_TAPS TAPS_BLUR2
#	define FILTER_BASE 0
#	define FILTER_COMBINE COMBINE_BLUR
#else  // 23, 24
#	define FILTER_CB CB_D
#	define FILTER_TAPS TAPS_NONE
#	define FILTER_BASE 0
#	define FILTER_COMBINE COMBINE_MATRIX
#endif
#ifndef FILTER_KNOCKOUT
#	define FILTER_KNOCKOUT 0
#endif
#define FILTER_MULTIPLY (FILTER == 4 || FILTER == 5 || FILTER == 6 || (FILTER >= 11 && FILTER <= 14) || FILTER == 17 || FILTER == 18 || FILTER == 20 || FILTER == 22 || FILTER == 24)

#if defined(CS_ORG_BINDLESS)

#	include "Scaleform/Filter.hlsli"

#	if FILTER_CB == CB_A
struct ScaleformFilterConstants
{
	float4 cxadd;
	float4 cxmul;
	float4 fsize;
	float2 offset;
	float2 offsetPadding;  // the cbuffer's gap before scolor
	float4 scolor;
	float4 scolor2;
	float2 srctexscale;
	float2 texscale;
};
#	elif FILTER_CB == CB_B
struct ScaleformFilterConstants
{
	float4 cxadd;
	float4 cxmul;
	float4 fsize;
	float2 offset;
	float2 offsetPadding;
	float4 scolor;
	float2 texscale;
};
#	elif FILTER_CB == CB_C
struct ScaleformFilterConstants
{
	float4 cxadd;
	float4 cxmul;
	float4 fsize;
	float2 texscale;
};
#	else
struct ScaleformFilterConstants
{
	float4 cxadd;
	float4 cxmulRows[4];  // the column-major float4x4's memory rows
};
#	endif
static ScaleformFilterConstants FilterConstants = FilterGeometry.Load<ScaleformFilterConstants>(FilterConstantsOffset);
static float4 cxadd = FilterConstants.cxadd;
#	if FILTER_CB == CB_D
// Each memory row of a column-major matrix is a column: the matrix whose columns they are.
static float4x4 cxmul = transpose(float4x4(FilterConstants.cxmulRows[0], FilterConstants.cxmulRows[1], FilterConstants.cxmulRows[2], FilterConstants.cxmulRows[3]));
#	else
static float4 cxmul = FilterConstants.cxmul;
static float4 fsize = FilterConstants.fsize;
static float2 texscale = FilterConstants.texscale;
#	endif
#	if FILTER_CB == CB_A || FILTER_CB == CB_B
static float2 offset = FilterConstants.offset;
static float4 scolor = FilterConstants.scolor;
#	endif
#	if FILTER_CB == CB_A
static float4 scolor2 = FilterConstants.scolor2;
static float2 srctexscale = FilterConstants.srctexscale;
static Texture2D srctex = ResourceDescriptorHeap[FilterSourceIndex];
static SamplerState srctex_s = SamplerDescriptorHeap[FilterSampler0Index];
static Texture2D tex = ResourceDescriptorHeap[FilterSecondIndex];
static SamplerState tex_s = SamplerDescriptorHeap[FilterSampler1Index];
#	else
static Texture2D tex = ResourceDescriptorHeap[FilterSourceIndex];
static SamplerState tex_s = SamplerDescriptorHeap[FilterSampler0Index];
#	endif

#else

#	if FILTER_CB == CB_A
cbuffer PSConstants
{
	float4 cxadd;
	float4 cxmul;
	float4 fsize;
	float2 offset;
	float4 scolor;
	float4 scolor2;
	float2 srctexscale;
	float2 texscale;
}
Texture2D srctex : register(t0);
sampler srctex_s : register(s0);
Texture2D tex : register(t1);
sampler tex_s : register(s1);
#	elif FILTER_CB == CB_B
cbuffer PSConstants
{
	float4 cxadd;
	float4 cxmul;
	float4 fsize;
	float2 offset;
	float4 scolor;
	float2 texscale;
}
Texture2D tex : register(t0);
sampler tex_s : register(s0);
#	elif FILTER_CB == CB_C
cbuffer PSConstants
{
	float4 cxadd;
	float4 cxmul;
	float4 fsize;
	float2 texscale;
}
Texture2D tex : register(t0);
sampler tex_s : register(s0);
#	else
cbuffer PSConstants
{
	float4 cxadd;
	float4x4 cxmul;
}
Texture2D tex : register(t0);
sampler tex_s : register(s0);
#	endif

#endif

/** @brief The variant's colour: the embedded source's statements, with its input as the parameter. */
float4 FilterColor(float2 tc)
{
	float4 fcolor;
#if FILTER_COMBINE == COMBINE_MATRIX
	float4 c = tex.Sample(tex_s, tc);
	fcolor = mul(c, cxmul) + cxadd * (c.a + cxadd.a);
#else
	float4 color = 0.0f;
#	if FILTER_TAPS == TAPS_BLUR1
	float i = 0.0f;
	for (i = -fsize.x; i <= fsize.x; i++)
		color += tex.SampleLevel(tex_s, tc + i * texscale, 0.0f);
#	else
	float2 i = 0.0f;
	for (i.x = -fsize.x; i.x <= fsize.x; i.x++)
		for (i.y = -fsize.y; i.y <= fsize.y; i.y++)
#		if FILTER_TAPS == TAPS_BOX
		{
			color += tex.SampleLevel(tex_s, tc + (offset + i) * texscale, 0.0f);
		}
#		elif FILTER_TAPS == TAPS_HIGHLIGHT
		{
			color.a += tex.SampleLevel(tex_s, tc + (offset + i) * texscale, 0.0f).a;
			color.r += tex.SampleLevel(tex_s, tc - (offset + i) * texscale, 0.0f).a;
		}
#		else
			color += tex.SampleLevel(tex_s, tc + i * texscale, 0.0f);
#		endif
#	endif
	fcolor = color * fsize.w;
#	if FILTER_BASE == 1
	float4 base = srctex.SampleLevel(srctex_s, tc * srctexscale, 0.0f);
#	elif FILTER_BASE == 2
	float4 base = srctex.Sample(srctex_s, tc * srctexscale);
#	endif
#	if FILTER_COMBINE == COMBINE_INNER
	fcolor = lerp(scolor, base, fcolor.a) * base.a;
#	elif FILTER_COMBINE == COMBINE_INNER_HIGHLIGHT
	fcolor.ar = saturate((1.0 - fcolor.ar) - (1.0 - fcolor.ra) * 0.5f);
	fcolor = (scolor * fcolor.a + scolor2 * fcolor.r + base * (1.0 - fcolor.a - fcolor.r)) * base.a;
#	elif FILTER_COMBINE == COMBINE_INNER_KNOCKOUT
	fcolor = scolor * (1.0 - fcolor.a) * base.a;
#	elif FILTER_COMBINE == COMBINE_OUTER
	fcolor = scolor * fcolor.a * (1.0 - base.a) + base;
#	elif FILTER_COMBINE == COMBINE_OUTER_HIGHLIGHT
	fcolor = (scolor * fcolor.a + scolor2 * fcolor.r) * (1.0 - base.a) + base;
#	elif FILTER_COMBINE == COMBINE_SHADOW
	fcolor = scolor * fcolor.a;
#	endif
#	if FILTER_KNOCKOUT
	fcolor *= (1.0 - base.a);
#	endif
	fcolor = fcolor * cxmul + cxadd * fcolor.a;
#endif
#if FILTER_MULTIPLY
	fcolor = lerp(1, fcolor, fcolor.a);
#endif
	return fcolor;
}

#if defined(CS_ORG_BINDLESS)
float4 main(ScaleformFilterVaryings input) : SV_Target
{
	return FilterColor(input.tc);
}
#else
void main(float2 tc : TEXCOORD0,
	out float4 fcolor : SV_Target)
{
	fcolor = FilterColor(tc);
}
#endif

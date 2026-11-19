// Scaleform GFx 3.3's fill pixel shaders (GRendererD3D1x, the exe's PS-A table), transcribed from the HLSL source the exe
// embeds beside the bytecode (docs/development/p5/scaleform.md, "Shaders"). FXC builds each variant as the game's own: the
// same signature, constants and resources, and the same instructions as the embedded source compiled by FXC.
//
// PS_KIND, the D3D11 renderer's pixel shader index (its member +0x240 + 8 * index):
//    1 solid colour                     2 bitmap (cxform x texture)           3 bitmap, multiply
//    4 glyph alpha (colour, texture a)  5 bitmap of DrawBitmaps (cxform x tex) 6 the same, multiply
//    7 YUV video                        8 YUV, multiply                       9 YUVA video   10 YUVA, multiply
//   11 gouraud (colour, factor alpha)  12 gouraud without factors            13 gouraud and one texture
//   14 gouraud and two textures        15-18: 11-14, multiply
// The multiply variants end with lerp(1, colour, alpha): the renderer picks them for the Multiply and Darken blend modes.
//
// The ORG build (CS_ORG_BINDLESS) reads the constants from the draw record and the textures and samplers from the
// descriptor heaps (Scaleform/Fill.hlsli); its inputs are every fill vertex stage's outputs, at fixed locations.

#ifndef PS_KIND
#	define PS_KIND 1
#endif

#if defined(CS_ORG_BINDLESS)
#	include "Scaleform/Fill.hlsli"
static float4 color = FillRecord.cxmul;  // the solid fill's constant
static float4 cxmul = FillRecord.cxmul;
static float4 cxadd = FillRecord.cxadd;
static Texture2D tex0 = ResourceDescriptorHeap[Texture0Index];
static Texture2D tex1 = ResourceDescriptorHeap[Texture1Index];
static SamplerState samp = SamplerDescriptorHeap[Sampler0Index];
static Texture2D tex_y = ResourceDescriptorHeap[Texture0Index];
static Texture2D tex_u = ResourceDescriptorHeap[Texture1Index];
static Texture2D tex_v = ResourceDescriptorHeap[Texture2Index];
static Texture2D tex_a = ResourceDescriptorHeap[Texture3Index];
static SamplerState samp_y = SamplerDescriptorHeap[Sampler0Index];
static SamplerState samp_u = SamplerDescriptorHeap[Sampler1Index];
static SamplerState samp_v = SamplerDescriptorHeap[Sampler2Index];
static SamplerState samp_a = SamplerDescriptorHeap[Sampler3Index];
#elif PS_KIND == 1
cbuffer PSConstants
{
	float4 color;
}
#elif PS_KIND == 2 || PS_KIND == 3 || PS_KIND >= 11
cbuffer PSConstants
{
	float4 cxmul;
	float4 cxadd;
}
Texture2D tex0;
Texture2D tex1;
sampler samp;
#elif PS_KIND >= 4 && PS_KIND <= 6
cbuffer PSConstants
{
	float4 cxmul;
	float4 cxadd;
}
Texture2D tex0 : register(t0);
sampler samp : register(s0);
#else  // 7-10: YUV
cbuffer PSConstants
{
	float4 cxmul;
	float4 cxadd;
}
Texture2D tex_y : register(t0);
Texture2D tex_u : register(t1);
Texture2D tex_v : register(t2);
Texture2D tex_a : register(t3);
sampler samp_y : register(s0);
sampler samp_u : register(s1);
sampler samp_v : register(s2);
sampler samp_a : register(s3);
#endif

/** @brief The variant's colour: the embedded source's statements, with its inputs as parameters. */
float4 FillColor(float4 color0, float4 factor, float2 tc0, float2 tc1)
{
	float4 ocolor;
#if PS_KIND == 1
	ocolor = color;
#elif PS_KIND == 2 || PS_KIND == 3
	float4 color = tex0.Sample(samp, tc0);
#	if PS_KIND == 2
	ocolor = color * cxmul + cxadd;
#	else
	color = color * cxmul + cxadd;
	ocolor = lerp(1, color, color.a);
#	endif
#elif PS_KIND == 4
	float4 color = color0;
	color = color * cxmul + cxadd;
	color.a = color.a * tex0.Sample(samp, tc0).a;
	ocolor = color;
#elif PS_KIND == 5
	float4 color = cxmul * tex0.Sample(samp, tc0) + cxadd;
	ocolor = color;
#elif PS_KIND == 6
	float4 color = cxmul * tex0.Sample(samp, tc0) + cxadd;
	ocolor = lerp(1, color, color.a);
#elif PS_KIND == 7 || PS_KIND == 8 || PS_KIND == 9 || PS_KIND == 10
	float Y = (float)tex_y.Sample(samp_y, tc0).a;
	float U = (float)tex_u.Sample(samp_u, tc0).a - 128.f / 255.f;
	float V = (float)tex_v.Sample(samp_v, tc0).a - 128.f / 255.f;
#	if PS_KIND == 7
	float3 color = (float3(Y, Y, Y) - float3(16.f / 255.f, 16.f / 255.f, 16.f / 255.f)) * 1.164;
	color.r += V * 1.596f;
	color.g += -U * 0.392f - V * 0.813f;
	color.b += U * 2.017f;
	ocolor = cxmul * float4(color, 1) + cxadd;
#	elif PS_KIND == 8
	float4 color = float4(float3((float3(Y, Y, Y) - float3(16.f / 255.f, 16.f / 255.f, 16.f / 255.f)) * 1.164), 1);
	color.r += V * 1.596f;
	color.g += -U * 0.392f - V * 0.813f;
	color.b += U * 2.017f;
	color = cxmul * color + cxadd;
	ocolor = lerp(1, color, color.a);
#	elif PS_KIND == 9
	float3 color = (float3(Y, Y, Y) - float3(16.f / 255.f, 16.f / 255.f, 16.f / 255.f)) * 1.164;
	color.r += V * 1.596f;
	color.g += -U * 0.392f - V * 0.813f;
	color.b += U * 2.017f;
	ocolor = cxmul * float4(color, tex_a.Sample(samp_a, tc0).a) + cxadd;
#	else
	float4 color = float4(float3((float3(Y, Y, Y) - float3(16.f / 255.f, 16.f / 255.f, 16.f / 255.f)) * 1.164), tex_a.Sample(samp_a, tc0).a);
	color.r += V * 1.596f;
	color.g += -U * 0.392f - V * 0.813f;
	color.b += U * 2.017f;
	color = cxmul * color + cxadd;
	ocolor = lerp(1, color, color.a);
#	endif
#elif PS_KIND == 11 || PS_KIND == 15
	float4 color = color0;
	color = color * cxmul + cxadd;
	color.a = color.a * factor.a;
#	if PS_KIND == 11
	ocolor = color;
#	else
	ocolor = lerp(1, color, color.a);
#	endif
#elif PS_KIND == 12
	ocolor = color0 * cxmul + cxadd;
#elif PS_KIND == 16
	float4 color = color0;
	color = color * cxmul + cxadd;
	ocolor = lerp(1, color, color.a);
#elif PS_KIND == 13 || PS_KIND == 17
	float4 color = color0;
	color = lerp(color, tex0.Sample(samp, tc0), factor.b);
	color = color * cxmul + cxadd;
	color.a = color.a * factor.a;
#	if PS_KIND == 13
	ocolor = color;
#	else
	ocolor = lerp(1, color, color.a);
#	endif
#elif PS_KIND == 14
	float4 color = lerp(tex1.Sample(samp, tc1), tex0.Sample(samp, tc0), factor.b);
	ocolor = color * cxmul + cxadd;
#elif PS_KIND == 18
	float4 color = lerp(tex1.Sample(samp, tc1), tex0.Sample(samp, tc0), factor.b);
	color = color * cxmul + cxadd;
	ocolor = lerp(1, color, color.a);
#endif
	return ocolor;
}

#if defined(CS_ORG_BINDLESS)

float4 main(ScaleformFillVaryings input) : SV_Target
{
	return FillColor(input.color, input.factor, input.tc0, input.tc1);
}

// The D3D11 signatures, as the embedded source declares them (their order fixes the input registers).
#elif PS_KIND == 1
void main(out float4 ocolor : SV_Target)
{
	ocolor = FillColor(0, 0, 0, 0);
}
#elif PS_KIND == 2 || PS_KIND == 3
void main(float2 tc0 : TEXCOORD0,
	out float4 ocolor : SV_Target)
{
	ocolor = FillColor(0, 0, tc0, 0);
}
#elif PS_KIND >= 4 && PS_KIND <= 6
void main(float4 color : COLOR,
	float2 tc0 : TEXCOORD0,
	out float4 ocolor : SV_Target)
{
	ocolor = FillColor(color, 0, tc0, 0);
}
#elif PS_KIND >= 7 && PS_KIND <= 10
void main(float4 unused : COLOR,
	float2 tc0 : TEXCOORD0,
	out float4 ocolor : SV_Target)
{
	ocolor = FillColor(unused, 0, tc0, 0);
}
#elif PS_KIND == 11 || PS_KIND == 15
void main(float4 color : COLOR,
	float4 factor : COLOR1,
	out float4 ocolor : SV_Target)
{
	ocolor = FillColor(color, factor, 0, 0);
}
#elif PS_KIND == 12 || PS_KIND == 16
void main(float4 color : COLOR,
	out float4 ocolor : SV_Target)
{
	ocolor = FillColor(color, 0, 0, 0);
}
#elif PS_KIND == 13 || PS_KIND == 17
void main(float4 color : COLOR,
	float4 factor : COLOR1,
	float2 tc0 : TEXCOORD0,
	out float4 ocolor : SV_Target)
{
	ocolor = FillColor(color, factor, tc0, 0);
}
#else  // 14, 18
void main(float4 color0 : COLOR,
	float4 factor : COLOR1,
	float2 tc0 : TEXCOORD0,
	float2 tc1 : TEXCOORD1,
	out float4 ocolor : SV_Target)
{
	ocolor = FillColor(color0, factor, tc0, tc1);
}
#endif

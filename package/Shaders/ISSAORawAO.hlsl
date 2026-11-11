// ISSAORawAO and ISSAORawAONoTemporal: the raw ambient obscurance pass. Both are built with
// MOTION_VECTORS; EDITOR is the build without temporal accumulation.
//
// Transcribed mechanically from the vanilla DXBC (Skyrim - Shaders.bsa), so its arithmetic, registers and
// constant layout are vanilla's. Temporaries hold raw bits (uint4), as DXBC registers do; integer work goes
// through asint/asuint and float work through asfloat. Checked against the vanilla shader on random inputs.

#include "Common/DummyVSTexCoord.hlsl"

#if defined(PSHADER)
#	if defined(EDITOR)
#		include "Common/FrameBuffer.hlsli"

SamplerState CS_Z_BufferSampler : register(s0);
SamplerState NormalsSampler : register(s1);
Texture2D<float4> CS_Z_BufferTex : register(t0);
Texture2D<float4> NormalsTex : register(t1);

// The engine fills these by name (the constructor's list) at vanilla's registers.
cbuffer PerGeometry : register(b2)
{
	float4 g_ProjInfos : packoffset(c0);
	float4 g_SSAOInfos : packoffset(c1);
	float4 g_ScreenInfos : packoffset(c2);
	float4 g_SSAOInfos2 : packoffset(c3);
};

void main(float4 v0 : SV_POSITION0, float2 v1 : TEXCOORD0, out float4 o0 : SV_Target0)
{
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;
	o0 = 0;
	r0.xy = asuint(v1.xy * g_ScreenInfos.xy);
	r0.zw = asuint((int2)(asfloat(r0.xy)));
	r1.xy = asuint(v1.xy * FrameBuffer::DynamicResolutionParams1.xy);
	r1.xy = asuint(max(asfloat(r1.xy), float2(0.0, 0.0)));
	r2.x = asuint(min(asfloat(r1.x), FrameBuffer::DynamicResolutionParams2.z));
	r2.yzw = asuint(min(asfloat(r1.yxy), FrameBuffer::DynamicResolutionParams1.yxy));
	{
		const float4 fetched = CS_Z_BufferTex.SampleLevel(CS_Z_BufferSampler, float2(asfloat(r2.x), asfloat(r2.y)), 0.0);
		r1.z = asuint(fetched.x);
	}
	r0.xy = asuint(trunc(asfloat(r0.xy)));
	r0.xy = asuint(asfloat(r0.xy) + float2(0.5, 0.5));
	r0.xy = asuint(asfloat(r0.xy) * g_ProjInfos.xy + g_ProjInfos.zw);
	r1.xy = asuint(asfloat(r1.zz) * asfloat(r0.xy));
	r0.x = asuint(saturate(asfloat(r1.z) * 0.00014285714));
	r0.y = asuint(asfloat(r0.x) * 256.0);
	r0.y = asuint(floor(asfloat(r0.y)));
	o0.y = asfloat(r0.y) * 0.00390625;
	o0.z = asfloat(r0.x) * 256.0 + -asfloat(r0.y);
	r0.y = asuint(asint(r0.z) * asint(0x00000003u));
	r1.w = asuint(asint(r0.z) * asint(r0.w) + asint(r0.w));
	r0.y = r0.y ^ r1.w;
	r0.y = asuint((float)(asint(r0.y)));
	r0.y = asuint(asfloat(r0.y) * 10.0);
	{
		const float4 fetched = NormalsTex.SampleGrad(NormalsSampler, float2(asfloat(r2.z), asfloat(r2.w)), float2(0.0, 0.0), float2(0.0, 0.0));
		r2.xy = asuint(fetched.xy);
	}
	r2.xy = asuint(asfloat(r2.xy) * float2(4.0, 4.0) + float2(-2.0, -2.0));
	r1.w = asuint(dot(asfloat(r2.xy), asfloat(r2.xy)));
	r3.zw = asuint(-asfloat(r1.ww) * float2(0.25, 0.5) + float2(1.0, 1.0));
	r1.w = asuint(sqrt(asfloat(r3.z)));
	r3.xy = asuint(asfloat(r1.ww) * asfloat(r2.xy));
	r2.xyz = asuint(asfloat(r3.xyw) * float3(-1.0, 1.0, -1.0));
	r1.w = asuint(g_SSAOInfos.y / asfloat(r1.z));
	r3.xyzw = asuint(v1.xyxy * float4(2.0, 2.0, 2.0, 2.0) + float4(-1.0, -1.0, -1.0, -1.0));
	r2.w = asuint(dot(asfloat(r3.xyzw), asfloat(r3.xyzw)));
	r3.x = asuint(asfloat(r0.x) + -0.3);
	r3.x = asuint(max(asfloat(r3.x), 0.0));
	r3.x = asuint(asfloat(r3.x) * 10.0 + g_SSAOInfos.z);
	r2.w = asuint(asfloat(r2.w) + asfloat(r3.x));
	r3.xy = uint2(0x00000000u, 0x00000000u);
	[loop] while (true)
	{
		r3.z = asuint(-(int)((asint(r3.y)) >= (asint(0x0000000fu))));
		if (r3.z != 0u)
			break;
		r3.z = asuint((float)(asint(r3.y)));
		r3.z = asuint(asfloat(r3.z) + 0.5);
		r3.w = asuint(asfloat(r1.w) * asfloat(r3.z));
		r3.z = asuint(asfloat(r3.z) * 3.3493335 + asfloat(r0.y));
		r4.x = asuint(sin(asfloat(r3.z)));
		r5.x = asuint(cos(asfloat(r3.z)));
		r3.z = asuint(asfloat(r3.w) * 0.06666667);
		r3.w = asuint(log2(asfloat(r3.z)));
		r3.w = asuint(floor(asfloat(r3.w)));
		r3.w = asuint((int)(asfloat(r3.w)));
		r3.w = asuint(asint(r3.w) + asint(0xfffffffdu));
		r3.w = asuint(max(asint(r3.w), asint(0x00000001u)));
		r3.w = asuint(min(asint(r3.w), asint(0x00000004u)));
		r5.y = r4.x;
		r4.xy = asuint(asfloat(r3.zz) * asfloat(r5.xy));
		r4.zw = asuint((int2)(asfloat(r4.xy)));
		r4.zw = asuint(asint(r0.zw) + asint(r4.zw));
		r4.xy = asuint(asfloat(r4.xy) * g_ScreenInfos.zw + v1.xy);
		r3.z = asuint((float)(asint(r3.w)));
		{
			const float4 fetched = CS_Z_BufferTex.SampleLevel(CS_Z_BufferSampler, float2(asfloat(r4.x), asfloat(r4.y)), asfloat(r3.z));
			r5.z = asuint(fetched.x);
		}
		r3.zw = asuint((float2)(asint(r4.zw)));
		r3.zw = asuint(asfloat(r3.zw) + float2(0.5, 0.5));
		r3.zw = asuint(asfloat(r3.zw) * g_ProjInfos.xy + g_ProjInfos.zw);
		r5.xy = asuint(asfloat(r5.zz) * asfloat(r3.zw));
		r4.xyz = asuint(-asfloat(r1.xyz) + asfloat(r5.xyz));
		r3.z = asuint(dot(asfloat(r4.xyz), asfloat(r4.xyz)));
		r3.w = asuint(dot(asfloat(r4.xyz), asfloat(r2.xyz)));
		r4.x = asuint(-asfloat(r3.z) + g_SSAOInfos2.y);
		r4.x = asuint(max(asfloat(r4.x), 0.0));
		r4.y = asuint(asfloat(r4.x) * asfloat(r4.x));
		r4.x = asuint(asfloat(r4.x) * asfloat(r4.y));
		r3.w = asuint(-asfloat(r2.w) + asfloat(r3.w));
		r3.z = asuint(asfloat(r3.z) + 0.01);
		r3.z = asuint(asfloat(r3.w) / asfloat(r3.z));
		r3.z = asuint(max(asfloat(r3.z), 0.0));
		r3.z = asuint(asfloat(r3.z) * asfloat(r4.x));
		r3.w = asuint(-(int)((asfloat(r5.z)) >= (g_SSAOInfos2.w)));
		r3.w = r3.w & 0x3f800000u;
		r3.x = asuint(asfloat(r3.z) * asfloat(r3.w) + asfloat(r3.x));
		r3.y = asuint(asint(r3.y) + asint(0x00000001u));
	}
	r0.y = asuint(asfloat(r3.x) * g_SSAOInfos.w);
	r0.y = asuint(-asfloat(r0.y) * 0.33333334 + 1.0);
	r0.y = asuint(max(asfloat(r0.y), 0.0));
	r1.x = asuint(max(g_SSAOInfos2.x, 1e-05));
	r1.x = asuint(min(asfloat(r1.x), 0.99999));
	r1.y = asuint(-(int)((asfloat(r1.x)) < (0.5)));
	r1.w = asuint(asfloat(r1.x) + asfloat(r1.x));
	r0.y = asuint(log2(asfloat(r0.y)));
	r1.w = asuint(asfloat(r0.y) * asfloat(r1.w));
	r1.w = asuint(exp2(asfloat(r1.w)));
	r1.x = asuint(asfloat(r1.x) + -0.5);
	r1.x = asuint(-asfloat(r1.x) * 2.0 + 1.0);
	r1.x = asuint(1.0 / asfloat(r1.x));
	r0.y = asuint(asfloat(r0.y) * asfloat(r1.x));
	r0.y = asuint(exp2(asfloat(r0.y)));
	r0.y = (r1.y != 0u ? r1.w : r0.y);
	r1.x = asuint(ddx_coarse(asfloat(r1.z)));
	r1.x = asuint(-(int)((abs(asfloat(r1.x))) < (0.02)));
	r1.y = asuint(ddx_coarse(asfloat(r0.y)));
	r0.zw = r0.zw & uint2(0x00000001u, 0x00000001u);
	r0.zw = asuint((float2)(asint(r0.zw)));
	r0.zw = asuint(asfloat(r0.zw) + float2(-0.5, -0.5));
	r0.z = asuint(-asfloat(r1.y) * asfloat(r0.z) + asfloat(r0.y));
	r0.y = (r1.x != 0u ? r0.z : r0.y);
	r0.z = asuint(ddy_coarse(asfloat(r1.z)));
	r0.z = asuint(-(int)((abs(asfloat(r0.z))) < (0.02)));
	r1.x = asuint(ddy_coarse(asfloat(r0.y)));
	r0.w = asuint(-asfloat(r1.x) * asfloat(r0.w) + asfloat(r0.y));
	r0.y = (r0.z != 0u ? r0.w : r0.y);
	r0.x = asuint(-(int)((asfloat(r0.x)) == (1.0)));
	o0.x = (r0.x != 0u ? 1.0 : asfloat(r0.y));
	o0.w = 1.0;
}
#	else
#		include "Common/FrameBuffer.hlsli"

SamplerState CS_Z_BufferSampler : register(s0);
SamplerState NormalsSampler : register(s1);
SamplerState AccumAOSampler : register(s2);
SamplerState MotionVectorsSampler : register(s3);
Texture2D<float4> CS_Z_BufferTex : register(t0);
Texture2D<float4> NormalsTex : register(t1);
Texture2D<float4> AccumAOTex : register(t2);
Texture2D<float4> MotionVectorsTex : register(t3);

// The engine fills these by name (the constructor's list) at vanilla's registers.
cbuffer PerGeometry : register(b2)
{
	float4 g_ProjInfos : packoffset(c0);
	float4 g_SSAOInfos : packoffset(c1);
	float4 g_ScreenInfos : packoffset(c2);
	float4 g_SSAOInfos2 : packoffset(c3);
};

void main(float4 v0 : SV_POSITION0, float2 v1 : TEXCOORD0, out float4 o0 : SV_Target0)
{
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0;
	o0 = 0;
	r0.xy = asuint(v1.xy * g_ScreenInfos.xy);
	r0.zw = asuint((int2)(asfloat(r0.xy)));
	r1.xy = asuint(v1.xy * FrameBuffer::DynamicResolutionParams1.xy);
	r1.xy = asuint(max(asfloat(r1.xy), float2(0.0, 0.0)));
	r2.x = asuint(FrameBuffer::DynamicResolutionParams2.z);
	r2.y = asuint(FrameBuffer::DynamicResolutionParams1.y);
	r1.zw = asuint(min(asfloat(r1.xy), asfloat(r2.xy)));
	{
		const float4 fetched = CS_Z_BufferTex.SampleLevel(CS_Z_BufferSampler, float2(asfloat(r1.z), asfloat(r1.w)), 0.0);
		r3.z = asuint(fetched.x);
	}
	r0.xy = asuint(trunc(asfloat(r0.xy)));
	r0.xy = asuint(asfloat(r0.xy) + float2(0.5, 0.5));
	r0.xy = asuint(asfloat(r0.xy) * g_ProjInfos.xy + g_ProjInfos.zw);
	r3.xy = asuint(asfloat(r3.zz) * asfloat(r0.xy));
	r0.x = asuint(saturate(asfloat(r3.z) * 0.00014285714));
	r0.y = asuint(asfloat(r0.x) * 256.0);
	r0.y = asuint(floor(asfloat(r0.y)));
	o0.y = asfloat(r0.y) * 0.00390625;
	o0.z = asfloat(r0.x) * 256.0 + -asfloat(r0.y);
	r0.y = asuint(-(int)((0.5) >= (asfloat(r0.x))));
	r0.y = r0.y & asuint(g_SSAOInfos.x);
	r2.z = asuint(asint(r0.z) * asint(0x00000003u));
	r2.w = asuint(asint(r0.z) * asint(r0.w) + asint(r0.w));
	r2.z = r2.w ^ r2.z;
	r2.z = asuint((float)(asint(r2.z)));
	r0.y = asuint(asfloat(r2.z) * 10.0 + asfloat(r0.y));
	r1.xy = asuint(min(asfloat(r1.xy), FrameBuffer::DynamicResolutionParams1.xy));
	{
		const float4 fetched = NormalsTex.SampleGrad(NormalsSampler, float2(asfloat(r1.x), asfloat(r1.y)), float2(0.0, 0.0), float2(0.0, 0.0));
		r1.xy = asuint(fetched.xy);
	}
	r1.xy = asuint(asfloat(r1.xy) * float2(4.0, 4.0) + float2(-2.0, -2.0));
	r2.z = asuint(dot(asfloat(r1.xy), asfloat(r1.xy)));
	r4.zw = asuint(-asfloat(r2.zz) * float2(0.25, 0.5) + float2(1.0, 1.0));
	r2.z = asuint(sqrt(asfloat(r4.z)));
	r4.xy = asuint(asfloat(r1.xy) * asfloat(r2.zz));
	r4.xyz = asuint(asfloat(r4.xyw) * float3(-1.0, 1.0, -1.0));
	r1.x = asuint(g_SSAOInfos.y / asfloat(r3.z));
	r5.xyzw = asuint(v1.xyxy * float4(2.0, 2.0, 2.0, 2.0) + float4(-1.0, -1.0, -1.0, -1.0));
	r1.y = asuint(dot(asfloat(r5.xyzw), asfloat(r5.xyzw)));
	r2.z = asuint(asfloat(r0.x) + -0.3);
	r2.z = asuint(max(asfloat(r2.z), 0.0));
	r2.z = asuint(asfloat(r2.z) * 10.0 + g_SSAOInfos.z);
	r1.y = asuint(asfloat(r1.y) + asfloat(r2.z));
	r2.zw = uint2(0x00000000u, 0x00000000u);
	[loop] while (true)
	{
		r3.w = asuint(-(int)((asint(r2.w)) >= (asint(0x00000005u))));
		if (r3.w != 0u)
			break;
		r3.w = asuint((float)(asint(r2.w)));
		r3.w = asuint(asfloat(r3.w) + 0.5);
		r4.w = asuint(asfloat(r1.x) * asfloat(r3.w));
		r3.w = asuint(asfloat(r3.w) * 2.512 + asfloat(r0.y));
		r5.x = asuint(sin(asfloat(r3.w)));
		r6.x = asuint(cos(asfloat(r3.w)));
		r3.w = asuint(asfloat(r4.w) * 0.2);
		r4.w = asuint(log2(asfloat(r3.w)));
		r4.w = asuint(floor(asfloat(r4.w)));
		r4.w = asuint((int)(asfloat(r4.w)));
		r4.w = asuint(asint(r4.w) + asint(0xfffffffdu));
		r4.w = asuint(max(asint(r4.w), asint(0x00000001u)));
		r4.w = asuint(min(asint(r4.w), asint(0x00000004u)));
		r6.y = r5.x;
		r5.xy = asuint(asfloat(r3.ww) * asfloat(r6.xy));
		r5.zw = asuint((int2)(asfloat(r5.xy)));
		r5.zw = asuint(asint(r0.zw) + asint(r5.zw));
		r5.xy = asuint(asfloat(r5.xy) * g_ScreenInfos.zw + v1.xy);
		r3.w = asuint((float)(asint(r4.w)));
		{
			const float4 fetched = CS_Z_BufferTex.SampleLevel(CS_Z_BufferSampler, float2(asfloat(r5.x), asfloat(r5.y)), asfloat(r3.w));
			r6.z = asuint(fetched.x);
		}
		r5.xy = asuint((float2)(asint(r5.zw)));
		r5.xy = asuint(asfloat(r5.xy) + float2(0.5, 0.5));
		r5.xy = asuint(asfloat(r5.xy) * g_ProjInfos.xy + g_ProjInfos.zw);
		r6.xy = asuint(asfloat(r6.zz) * asfloat(r5.xy));
		r5.xyz = asuint(-asfloat(r3.xyz) + asfloat(r6.xyz));
		r3.w = asuint(dot(asfloat(r5.xyz), asfloat(r5.xyz)));
		r4.w = asuint(dot(asfloat(r5.xyz), asfloat(r4.xyz)));
		r5.x = asuint(-asfloat(r3.w) + g_SSAOInfos2.y);
		r5.x = asuint(max(asfloat(r5.x), 0.0));
		r5.y = asuint(asfloat(r5.x) * asfloat(r5.x));
		r5.x = asuint(asfloat(r5.x) * asfloat(r5.y));
		r4.w = asuint(-asfloat(r1.y) + asfloat(r4.w));
		r3.w = asuint(asfloat(r3.w) + 0.01);
		r3.w = asuint(asfloat(r4.w) / asfloat(r3.w));
		r3.w = asuint(max(asfloat(r3.w), 0.0));
		r3.w = asuint(asfloat(r3.w) * asfloat(r5.x));
		r4.w = asuint(-(int)((asfloat(r6.z)) >= (g_SSAOInfos2.w)));
		r4.w = r4.w & 0x3f800000u;
		r2.z = asuint(asfloat(r3.w) * asfloat(r4.w) + asfloat(r2.z));
		r2.w = asuint(asint(r2.w) + asint(0x00000001u));
	}
	r0.y = asuint(-asfloat(r2.z) * g_SSAOInfos.w + 1.0);
	r0.y = asuint(max(asfloat(r0.y), 0.0));
	r1.x = asuint(max(g_SSAOInfos2.x, 1e-05));
	r1.x = asuint(min(asfloat(r1.x), 0.99999));
	r1.y = asuint(-(int)((asfloat(r1.x)) < (0.5)));
	r2.z = asuint(asfloat(r1.x) + asfloat(r1.x));
	r0.y = asuint(log2(asfloat(r0.y)));
	r2.z = asuint(asfloat(r0.y) * asfloat(r2.z));
	r2.z = asuint(exp2(asfloat(r2.z)));
	r1.x = asuint(asfloat(r1.x) + -0.5);
	r1.x = asuint(-asfloat(r1.x) * 2.0 + 1.0);
	r1.x = asuint(1.0 / asfloat(r1.x));
	r0.y = asuint(asfloat(r0.y) * asfloat(r1.x));
	r0.y = asuint(exp2(asfloat(r0.y)));
	r0.y = (r1.y != 0u ? r2.z : r0.y);
	r1.x = asuint(ddx_coarse(asfloat(r3.z)));
	r1.x = asuint(-(int)((abs(asfloat(r1.x))) < (0.02)));
	r1.y = asuint(ddx_coarse(asfloat(r0.y)));
	r0.zw = r0.zw & uint2(0x00000001u, 0x00000001u);
	r0.zw = asuint((float2)(asint(r0.zw)));
	r0.zw = asuint(asfloat(r0.zw) + float2(-0.5, -0.5));
	r0.z = asuint(-asfloat(r1.y) * asfloat(r0.z) + asfloat(r0.y));
	r0.y = (r1.x != 0u ? r0.z : r0.y);
	r0.z = asuint(ddy_coarse(asfloat(r3.z)));
	r0.z = asuint(-(int)((abs(asfloat(r0.z))) < (0.02)));
	r1.x = asuint(ddy_coarse(asfloat(r0.y)));
	r0.w = asuint(-asfloat(r1.x) * asfloat(r0.w) + asfloat(r0.y));
	r0.y = (r0.z != 0u ? r0.w : r0.y);
	{
		const float4 fetched = MotionVectorsTex.Sample(MotionVectorsSampler, float2(asfloat(r1.z), asfloat(r1.w)));
		r0.zw = asuint(fetched.xy);
	}
	r1.x = asuint(dot(asfloat(r0.zw), asfloat(r0.zw)));
	r1.x = asuint(rsqrt(asfloat(r1.x)));
	r1.xy = asuint(asfloat(r0.zw) * asfloat(r1.xx));
	r1.xy = asuint(asfloat(r1.xy) * float2(0.055, 0.055) + v1.xy);
	r1.xy = asuint(asfloat(r1.xy) * FrameBuffer::DynamicResolutionParams1.xy);
	r1.xy = asuint(max(asfloat(r1.xy), float2(0.0, 0.0)));
	r1.xy = asuint(min(asfloat(r2.xy), asfloat(r1.xy)));
	{
		const float4 fetched = MotionVectorsTex.Sample(MotionVectorsSampler, float2(asfloat(r1.x), asfloat(r1.y)));
		r1.xy = asuint(fetched.xy);
	}
	r1.zw = asuint(asfloat(r0.zw) + v1.xy);
	r1.zw = asuint(asfloat(r1.zw) * FrameBuffer::DynamicResolutionParams1.zw);
	r1.zw = asuint(max(asfloat(r1.zw), float2(0.0, 0.0)));
	r2.x = asuint(min(asfloat(r1.z), FrameBuffer::DynamicResolutionParams2.w));
	r2.y = asuint(min(asfloat(r1.w), FrameBuffer::DynamicResolutionParams1.w));
	{
		const float4 fetched = AccumAOTex.Sample(AccumAOSampler, float2(asfloat(r2.x), asfloat(r2.y)));
		r2.xyz = asuint(fetched.xyz);
	}
	r1.z = asuint(dot(asfloat(r2.yz), float2(0.99610895, 0.0038910506)));
	r1.z = asuint(asfloat(r0.x) + -asfloat(r1.z));
	r1.z = asuint(-abs(asfloat(r1.z)) * 200.0 + 1.0);
	r1.z = asuint(max(asfloat(r1.z), 0.0));
	r0.zw = asuint(asfloat(r0.zw) + -asfloat(r1.xy));
	r0.z = asuint(dot(asfloat(r0.zw), asfloat(r0.zw)));
	r0.z = asuint(sqrt(asfloat(r0.z)));
	r0.z = asuint(-asfloat(r0.z) * 200.0 + 1.0);
	r0.z = asuint(max(asfloat(r0.z), 0.2));
	r0.w = asuint(asfloat(r0.y) + -asfloat(r2.x));
	r0.w = asuint(saturate(-g_SSAOInfos2.z * abs(asfloat(r0.w)) + 1.0));
	r0.w = asuint(asfloat(r0.w) * asfloat(r1.z));
	r0.z = asuint(asfloat(r0.z) * asfloat(r0.w));
	r0.w = asuint(-asfloat(r0.y) + asfloat(r2.x));
	r0.y = asuint(asfloat(r0.z) * asfloat(r0.w) + asfloat(r0.y));
	r0.x = asuint(-(int)((asfloat(r0.x)) == (1.0)));
	o0.x = (r0.x != 0u ? 1.0 : asfloat(r0.y));
	o0.w = 1.0;
}
#	endif
#endif

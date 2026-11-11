// ISSILRawInd: the raw indirect lighting pass of the screen-space indirect lighting composite.
//
// Transcribed mechanically from the vanilla DXBC (Skyrim - Shaders.bsa), so its arithmetic, registers and
// constant layout are vanilla's. Temporaries hold raw bits (uint4), as DXBC registers do; integer work goes
// through asint/asuint and float work through asfloat. Checked against the vanilla shader on random inputs.

#include "Common/DummyVSTexCoord.hlsl"

#if defined(PSHADER)
SamplerState CS_Z_BufferSampler : register(s0);
SamplerState NormalsSampler : register(s1);
SamplerState MotionVectorsSampler : register(s2);
SamplerState EmissiveColorSampler : register(s3);
SamplerState AccumIndSampler : register(s4);
Texture2D<float4> CS_Z_BufferTex : register(t0);
Texture2D<float4> NormalsTex : register(t1);
Texture2D<float4> MotionVectorsTex : register(t2);
Texture2D<float4> EmissiveColorTex : register(t3);
Texture2D<float4> AccumIndTex : register(t4);

// The engine fills these by name (the constructor's list) at vanilla's registers.
cbuffer PerGeometry : register(b2)
{
	float4 g_ProjInfos : packoffset(c0);
	float4 g_IndInfos : packoffset(c1);
	float4 g_ScreenInfos : packoffset(c2);
	float4 g_IndInfos2 : packoffset(c3);
};

void main(float4 v0 : SV_POSITION0, float2 v1 : TEXCOORD0, out float4 o0 : SV_Target0)
{
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0;
	o0 = 0;
	r0.xy = asuint(v1.xy * g_ScreenInfos.xy);
	r0.zw = asuint((int2)(asfloat(r0.yx)));
	{
		const float4 fetched = CS_Z_BufferTex.Sample(CS_Z_BufferSampler, float2(v1.x, v1.y));
		r1.z = asuint(fetched.x);
	}
	r0.xy = asuint(trunc(asfloat(r0.xy)));
	r0.xy = asuint(asfloat(r0.xy) + float2(0.5, 0.5));
	r0.xy = asuint(asfloat(r0.xy) * g_ProjInfos.xy + g_ProjInfos.zw);
	r1.xy = asuint(asfloat(r1.zz) * asfloat(r0.xy));
	r0.x = asuint(saturate(asfloat(r1.z) * 0.00014285714));
	r0.y = asuint(-(int)((0.5) >= (asfloat(r0.x))));
	r0.y = r0.y & asuint(g_IndInfos.x);
	r1.w = asuint(asint(r0.w) * asint(0x00000003u));
	r2.x = asuint(asint(r0.w) * asint(r0.z) + asint(r0.z));
	r1.w = r1.w ^ r2.x;
	r1.w = asuint(asint(r1.w) * asint(0x0000000au));
	r1.w = asuint((float)(asint(r1.w)));
	r0.y = asuint(asfloat(r0.y) + asfloat(r1.w));
	r2.xyz = asuint(ddy_coarse(asfloat(r1.zxy)));
	r3.xyz = asuint(ddx_coarse(asfloat(r1.yzx)));
	r4.xyz = asuint(asfloat(r2.xyz) * asfloat(r3.xyz));
	r2.xyz = asuint(asfloat(r2.zxy) * asfloat(r3.yzx) + -asfloat(r4.xyz));
	r1.w = asuint(dot(asfloat(r2.xyz), asfloat(r2.xyz)));
	r1.w = asuint(rsqrt(asfloat(r1.w)));
	r2.xyz = asuint(asfloat(r1.www) * asfloat(r2.xyz));
	{
		const float4 fetched = NormalsTex.SampleGrad(NormalsSampler, float2(v1.x, v1.y), float2(0.0, 0.0), float2(0.0, 0.0));
		r3.xy = asuint(fetched.xy);
	}
	r3.xy = asuint(asfloat(r3.xy) * float2(4.0, 4.0) + float2(-2.0, -2.0));
	r1.w = asuint(dot(asfloat(r3.xy), asfloat(r3.xy)));
	r4.zw = asuint(-asfloat(r1.ww) * float2(0.25, 0.5) + float2(1.0, 1.0));
	r1.w = asuint(sqrt(asfloat(r4.z)));
	r4.xy = asuint(asfloat(r1.ww) * asfloat(r3.xy));
	r3.xyz = asuint(asfloat(r4.xyw) * float3(-1.0, 1.0, -1.0) + -asfloat(r2.xyz));
	r2.xyz = asuint(g_IndInfos2.xxx * asfloat(r3.xyz) + asfloat(r2.xyz));
	r1.w = asuint(g_IndInfos.y * 100.0);
	r1.w = asuint(asfloat(r1.w) / asfloat(r1.z));
	r3.xy = asuint(v1.xy * float2(2.0, 2.0) + float2(-1.0, -1.0));
	r2.w = asuint(dot(abs(asfloat(r3.xy)), abs(asfloat(r3.xy))));
	r3.x = asuint(asfloat(r0.x) + -0.3);
	r3.x = asuint(max(asfloat(r3.x), 0.0));
	r3.x = asuint(asfloat(r3.x) * 10.0 + g_IndInfos.z);
	r2.w = asuint(asfloat(r2.w) * 5.0 + asfloat(r3.x));
	r3.x = asuint(g_IndInfos.y * g_IndInfos.y);
	r3.yzw = uint3(0x00000000u, 0x00000000u, 0x00000000u);
	r4.x = 0x00000000u;
	[loop] while (true) {
		r4.y = asuint(-(int)((asint(r4.x)) >= (asint(0x0000000fu))));
		if (r4.y != 0u) break;
		r4.y = asuint((float)(asint(r4.x)));
		r4.y = asuint(asfloat(r4.y) + 0.5);
		r4.z = asuint(asfloat(r1.w) * asfloat(r4.y));
		r4.y = asuint(asfloat(r4.y) * 3.3493335 + asfloat(r0.y));
		r5.x = asuint(sin(asfloat(r4.y)));
		r6.x = asuint(cos(asfloat(r4.y)));
		r4.y = asuint(asfloat(r4.z) * 0.06666667);
		r4.z = asuint(log2(asfloat(r4.y)));
		r4.z = asuint(floor(asfloat(r4.z)));
		r4.z = asuint((int)(asfloat(r4.z)));
		r4.z = asuint(asint(r4.z) + asint(0xfffffffdu));
		r4.z = asuint(max(asint(r4.z), asint(0x00000000u)));
		r4.z = asuint(min(asint(r4.z), asint(0x00000004u)));
		r6.y = r5.x;
		r4.yw = asuint(asfloat(r4.yy) * asfloat(r6.xy));
		r5.xy = asuint((int2)(asfloat(r4.wy)));
		r5.xy = asuint(asint(r0.zw) + asint(r5.xy));
		r4.yw = asuint(asfloat(r4.yw) * g_ScreenInfos.zw + v1.xy);
		r4.z = asuint((float)(asint(r4.z)));
		{
			const float4 fetched = CS_Z_BufferTex.SampleLevel(CS_Z_BufferSampler, float2(asfloat(r4.y), asfloat(r4.w)), asfloat(r4.z));
			r6.y = asuint(fetched.x);
		}
		r5.xy = asuint((float2)(asint(r5.xy)));
		r5.xy = asuint(asfloat(r5.xy) + float2(0.5, 0.5));
		r5.xy = asuint(asfloat(r5.xy) * g_ProjInfos.yx + g_ProjInfos.wz);
		r6.xz = asuint(asfloat(r6.yy) * asfloat(r5.xy));
		r5.xyz = asuint(-asfloat(r1.xyz) + asfloat(r6.zxy));
		r4.z = asuint(dot(asfloat(r5.xyz), asfloat(r5.xyz)));
		r5.w = asuint(sqrt(asfloat(r4.z)));
		r6.w = asuint(dot(asfloat(r5.xyz), asfloat(r2.xyz)));
		r7.x = asuint(g_IndInfos.y * g_IndInfos.y + -asfloat(r4.z));
		r7.x = asuint(max(asfloat(r7.x), 0.0));
		r7.yzw = asuint(ddy_coarse(asfloat(r6.yzx)));
		r6.xyz = asuint(ddx_coarse(asfloat(r6.xyz)));
		r8.xyz = asuint(asfloat(r6.xyz) * asfloat(r7.yzw));
		r6.xyz = asuint(asfloat(r7.wyz) * asfloat(r6.yzx) + -asfloat(r8.xyz));
		r7.y = asuint(dot(asfloat(r6.xyz), asfloat(r6.xyz)));
		r7.y = asuint(rsqrt(asfloat(r7.y)));
		r6.xyz = asuint(asfloat(r6.xyz) * asfloat(r7.yyy));
		r5.x = asuint(dot(asfloat(r5.xyz), asfloat(r6.xyz)));
		r5.y = asuint(asfloat(r7.x) * asfloat(r7.x));
		r5.y = asuint(asfloat(r7.x) * asfloat(r5.y));
		r5.x = asuint(asfloat(r6.w) * asfloat(r5.x) + -asfloat(r2.w));
		r4.z = asuint(asfloat(r4.z) * asfloat(r4.z) + 0.01);
		r4.z = asuint(asfloat(r5.x) / asfloat(r4.z));
		r4.z = asuint(max(asfloat(r4.z), 0.0));
		r4.z = asuint(asfloat(r4.z) * asfloat(r5.y));
		{
			const float4 fetched = EmissiveColorTex.Sample(EmissiveColorSampler, float2(asfloat(r4.y), asfloat(r4.w)));
			r5.xyz = asuint(fetched.xyz);
		}
		r4.y = asuint(saturate(asfloat(r5.w) / g_IndInfos.y));
		r4.y = asuint(-asfloat(r4.y) + 1.0);
		r4.y = asuint(asfloat(r4.y) * asfloat(r4.z));
		r3.yzw = asuint(asfloat(r4.yyy) * asfloat(r5.xyz) + asfloat(r3.yzw));
		r4.x = asuint(asint(r4.x) + asint(0x00000001u));
	}
	r0.y = asuint(asfloat(r3.x) * g_IndInfos.y);
	r0.y = asuint(asfloat(r0.y) * asfloat(r0.y));
	r0.yzw = asuint(asfloat(r3.yzw) / asfloat(r0.yyy));
	r0.yzw = asuint(asfloat(r0.yzw) * g_IndInfos.www);
	r0.yzw = asuint(asfloat(r0.yzw) * float3(0.33333334, 0.33333334, 0.33333334));
	r0.yzw = asuint(max(asfloat(r0.yzw), float3(0.0, 0.0, 0.0)));
	{
		const float4 fetched = MotionVectorsTex.Sample(MotionVectorsSampler, float2(v1.x, v1.y));
		r1.xy = asuint(fetched.xy);
	}
	r1.xy = asuint(asfloat(r1.xy) + v1.xy);
	{
		const float4 fetched = AccumIndTex.Sample(AccumIndSampler, float2(asfloat(r1.x), asfloat(r1.y)));
		r2.xyzw = asuint(fetched.xyzw);
	}
	r1.w = asuint(asfloat(r0.x) + -asfloat(r2.w));
	r1.w = asuint(-abs(asfloat(r1.w)) * 200.0 + 1.0);
	r1.w = asuint(max(asfloat(r1.w), 0.0));
	r3.xy = asuint(-(int2)((float2(0.99, 0.99)) < (asfloat(r1.xy))));
	r2.w = r3.y | r3.x;
	r1.xy = asuint(-(int2)((asfloat(r1.xy)) < (float2(0.01, 0.01))));
	r1.x = r1.x | r2.w;
	r1.x = r1.y | r1.x;
	r3.xyz = asuint(asfloat(r0.yzw) + -asfloat(r2.xyz));
	r3.xyz = asuint(-abs(asfloat(r3.xyz)) * float3(0.7, 0.7, 0.7) + float3(1.0, 1.0, 1.0));
	r3.xyz = asuint(max(asfloat(r3.xyz), float3(0.0, 0.0, 0.0)));
	r1.y = asuint(asfloat(r3.y) + asfloat(r3.x));
	r1.y = asuint(asfloat(r3.z) + asfloat(r1.y));
	r1.y = asuint(asfloat(r1.w) * asfloat(r1.y));
	r1.y = asuint(asfloat(r1.y) * 0.3);
	r1.x = (r1.x != 0u ? 0x00000000u : r1.y);
	r2.xyz = asuint(-asfloat(r0.yzw) + asfloat(r2.xyz));
	r0.yzw = asuint(asfloat(r1.xxx) * asfloat(r2.xyz) + asfloat(r0.yzw));
	r1.x = asuint(asfloat(r1.z) + -10000.0);
	r1.x = asuint(saturate(asfloat(r1.x) * -0.0001));
	r1.y = asuint(asfloat(r1.x) * -2.0 + 3.0);
	r1.x = asuint(asfloat(r1.x) * asfloat(r1.x));
	r1.x = asuint(asfloat(r1.x) * asfloat(r1.y));
	o0.xyz = asfloat(r0.yzw) * asfloat(r1.xxx);
	o0.w = asfloat(r0.x);
}
#endif

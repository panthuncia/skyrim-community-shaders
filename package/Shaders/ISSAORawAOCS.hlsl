// ISSAORawAOCS: the compute raw ambient obscurance pass.
//
// Transcribed mechanically from the vanilla DXBC (Skyrim - Shaders.bsa), so its arithmetic, registers and
// constant layout are vanilla's. Temporaries hold raw bits (uint4), as DXBC registers do; integer work goes
// through asint/asuint and float work through asfloat. Checked against the vanilla shader on random inputs.

SamplerState CS_Z_BufferSampler : register(s0);
SamplerState AccumAOSampler : register(s1);
SamplerState MotionVectorsSampler : register(s2);
Texture2D<float4> CS_Z_BufferTex : register(t0);
Texture2D<float4> AccumAOTex : register(t1);
Texture2D<float4> MotionVectorsTex : register(t2);
RWTexture2D<float4> g_OutputBuffer : register(u0);

// The engine uploads this block whole (SetComputeShaderConstants); its constant list is
// g_ProjInfos, g_SSAOInfos, g_ScreenInfos, gDynamicResolutionInfo.
cbuffer PerTechnique : register(b0)
{
	float4 cb0[4];
};

[numthreads(16, 16, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0;
	r0.xy = asuint((float2)(dispatchID.xy));
	r0.xy = asuint(asfloat(r0.xy) * cb0[2].zw);
	r0.xy = asuint(cb0[2].zw * float2(0.5, 0.5) + asfloat(r0.xy));
	r0.zw = asuint(asfloat(r0.xy) * cb0[3].xy);
	{
		const float4 fetched = CS_Z_BufferTex.SampleLevel(CS_Z_BufferSampler, float2(asfloat(r0.z), asfloat(r0.w)), 0.0);
		r1.z = asuint(fetched.x);
	}
	r2.xy = asuint((float2)(asint(dispatchID.xy)));
	r2.xy = asuint(asfloat(r2.xy) + float2(0.5, 0.5));
	r2.zw = asuint(asfloat(r2.xy) * cb0[0].xy + cb0[0].zw);
	r1.xy = asuint(asfloat(r1.zz) * asfloat(r2.zw));
	r1.w = asuint(saturate(asfloat(r1.z) * 0.00014285714));
	r2.z = asuint(asfloat(r1.w) * 256.0);
	r2.z = asuint(floor(asfloat(r2.z)));
	r3.y = asuint(asfloat(r2.z) * 0.00390625);
	r3.z = asuint(asfloat(r1.w) * 256.0 + -asfloat(r2.z));
	r2.z = asuint(-(int)((0.5) >= (asfloat(r1.w))));
	r2.z = r2.z & asuint(cb0[1].x);
	r2.w = asuint(asint(dispatchID.x) * asint(0x00000003u));
	r4.x = asuint(asint(dispatchID.x) * asint(dispatchID.y) + asint(dispatchID.y));
	r2.w = r2.w ^ r4.x;
	r2.w = asuint(asint(r2.w) * asint(0x0000000au));
	r2.w = asuint((float)(r2.w));
	r2.z = asuint(asfloat(r2.z) + asfloat(r2.w));
	r4.xyzw = asuint(asint(dispatchID.xyyx) + int4(asint(0xffffffffu), asint(0xffffffffu), asint(0x00000001u), asint(0x00000001u)));
	r5.xy = r4.xy;
	r5.z = 0x00000000u;
	{
		const float4 fetched = CS_Z_BufferTex.Load(int3(asint(r5.x), asint(r5.y), asint(r5.z)));
		r2.w = asuint(fetched.x);
	}
	r5.w = dispatchID.y;
	{
		const float4 fetched = CS_Z_BufferTex.Load(int3(asint(r5.x), asint(r5.w), asint(r5.z)));
		r4.x = asuint(fetched.x);
	}
	r6.xy = (uint2)(cb0[2].yx);
	r6.xy = min(r4.zw, r6.xy);
	r6.zw = r5.xz;
	{
		const float4 fetched = CS_Z_BufferTex.Load(int3(asint(r6.z), asint(r6.x), asint(r6.w)));
		r4.y = asuint(fetched.x);
	}
	r5.x = dispatchID.x;
	{
		const float4 fetched = CS_Z_BufferTex.Load(int3(asint(r5.x), asint(r5.y), asint(r5.z)));
		r4.z = asuint(fetched.x);
	}
	r5.w = r6.x;
	{
		const float4 fetched = CS_Z_BufferTex.Load(int3(asint(r5.x), asint(r5.w), asint(r5.z)));
		r4.w = asuint(fetched.x);
	}
	r5.xz = r6.yw;
	{
		const float4 fetched = CS_Z_BufferTex.Load(int3(asint(r5.x), asint(r5.y), asint(r5.z)));
		r5.y = asuint(fetched.x);
	}
	r5.w = dispatchID.y;
	{
		const float4 fetched = CS_Z_BufferTex.Load(int3(asint(r5.x), asint(r5.w), asint(r5.z)));
		r5.x = asuint(fetched.x);
	}
	{
		const float4 fetched = CS_Z_BufferTex.Load(int3(asint(r6.y), asint(r6.x), asint(r6.w)));
		r5.z = asuint(fetched.x);
	}
	r4.x = asuint(-asfloat(r4.x) * 2.0 + -asfloat(r2.w));
	r4.x = asuint(-asfloat(r4.y) + asfloat(r4.x));
	r4.x = asuint(asfloat(r5.y) + asfloat(r4.x));
	r4.x = asuint(asfloat(r5.x) * 2.0 + asfloat(r4.x));
	r4.x = asuint(asfloat(r5.z) + asfloat(r4.x));
	r2.w = asuint(-asfloat(r4.z) * 2.0 + -asfloat(r2.w));
	r2.w = asuint(-asfloat(r5.y) + asfloat(r2.w));
	r2.w = asuint(asfloat(r4.y) + asfloat(r2.w));
	r2.w = asuint(asfloat(r4.w) * 2.0 + asfloat(r2.w));
	r5.y = asuint(asfloat(r5.z) + asfloat(r2.w));
	r5.x = asuint(-asfloat(r4.x));
	r5.z = 0x3f800000u;
	r2.w = asuint(dot(asfloat(r5.xyz), asfloat(r5.xyz)));
	r4.z = asuint(rsqrt(asfloat(r2.w)));
	r4.xy = asuint(asfloat(r4.zz) * asfloat(r5.xy));
	r4.xyz = asuint(asfloat(r4.xyz) * float3(0.5, 0.5, 0.5) + float3(0.5, 0.5, 0.5));
	r2.w = asuint(cb0[1].y * 100.0);
	r2.w = asuint(asfloat(r2.w) / asfloat(r1.z));
	r5.xy = asuint(asfloat(r0.zw) * float2(2.0, 2.0) + float2(-1.0, -1.0));
	r4.w = asuint(dot(abs(asfloat(r5.xy)), abs(asfloat(r5.xy))));
	r5.x = asuint(asfloat(r1.w) + -0.3);
	r5.x = asuint(max(asfloat(r5.x), 0.0));
	r5.x = asuint(asfloat(r5.x) * 10.0 + cb0[1].z);
	r4.w = asuint(asfloat(r4.w) * 5.0 + asfloat(r5.x));
	r5.x = asuint(cb0[1].y * cb0[1].y);
	r5.yz = uint2(0x00000000u, 0x00000000u);
	[loop] while (true) {
		r5.w = asuint(-(int)((asint(r5.z)) >= (asint(0x00000005u))));
		if (r5.w != 0u) break;
		r5.w = asuint((float)(asint(r5.z)));
		r5.w = asuint(asfloat(r5.w) + 0.5);
		r6.x = asuint(asfloat(r2.w) * asfloat(r5.w));
		r5.w = asuint(asfloat(r5.w) * 2.512 + asfloat(r2.z));
		r7.x = asuint(sin(asfloat(r5.w)));
		r8.x = asuint(cos(asfloat(r5.w)));
		r5.w = asuint(asfloat(r6.x) * 0.2);
		r6.x = asuint(log2(asfloat(r5.w)));
		r6.x = asuint(floor(asfloat(r6.x)));
		r6.x = asuint((int)(asfloat(r6.x)));
		r6.x = asuint(asint(r6.x) + asint(0xfffffffdu));
		r6.x = asuint(max(asint(r6.x), asint(0x00000000u)));
		r6.x = asuint(min(asint(r6.x), asint(0x00000002u)));
		r8.y = r7.x;
		r6.yz = asuint(asfloat(r5.ww) * asfloat(r8.xy));
		r7.xy = asuint((int2)(asfloat(r6.yz)));
		r7.xy = asuint(asint(r7.xy) + asint(dispatchID.xy));
		r6.yz = asuint(trunc(asfloat(r6.yz)));
		r6.yz = asuint(asfloat(r2.xy) + asfloat(r6.yz));
		r6.yz = asuint(asfloat(r6.yz) * cb0[2].zw);
		r6.yz = asuint(asfloat(r6.yz) * cb0[3].xy);
		r5.w = asuint((float)(asint(r6.x)));
		{
			const float4 fetched = CS_Z_BufferTex.SampleLevel(CS_Z_BufferSampler, float2(asfloat(r6.y), asfloat(r6.z)), asfloat(r5.w));
			r6.z = asuint(fetched.x);
		}
		r7.xy = asuint((float2)(asint(r7.xy)));
		r7.xy = asuint(asfloat(r7.xy) + float2(0.5, 0.5));
		r7.xy = asuint(asfloat(r7.xy) * cb0[0].xy + cb0[0].zw);
		r6.xy = asuint(asfloat(r6.zz) * asfloat(r7.xy));
		r6.xyz = asuint(-asfloat(r1.xyz) + asfloat(r6.xyz));
		r5.w = asuint(dot(asfloat(r6.xyz), asfloat(r6.xyz)));
		r6.x = asuint(dot(asfloat(r6.xyz), asfloat(r4.xyz)));
		r6.y = asuint(cb0[1].y * cb0[1].y + -asfloat(r5.w));
		r6.y = asuint(max(asfloat(r6.y), 0.0));
		r6.z = asuint(asfloat(r6.y) * asfloat(r6.y));
		r6.y = asuint(asfloat(r6.y) * asfloat(r6.z));
		r6.x = asuint(-asfloat(r4.w) + asfloat(r6.x));
		r5.w = asuint(asfloat(r5.w) + 0.01);
		r5.w = asuint(asfloat(r6.x) / asfloat(r5.w));
		r5.w = asuint(max(asfloat(r5.w), 0.0));
		r5.y = asuint(asfloat(r6.y) * asfloat(r5.w) + asfloat(r5.y));
		r5.z = asuint(asint(r5.z) + asint(0x00000001u));
	}
	r1.x = asuint(asfloat(r5.x) * cb0[1].y);
	r1.x = asuint(asfloat(r1.x) * asfloat(r1.x));
	r1.x = asuint(asfloat(r5.y) / asfloat(r1.x));
	r1.x = asuint(-asfloat(r1.x) * cb0[1].w + 1.0);
	r1.x = asuint(max(asfloat(r1.x), 0.0));
	{
		const float4 fetched = MotionVectorsTex.SampleLevel(MotionVectorsSampler, float2(asfloat(r0.z), asfloat(r0.w)), 0.0);
		r1.yz = asuint(fetched.xy);
	}
	r2.x = asuint(dot(asfloat(r1.yz), asfloat(r1.yz)));
	r2.x = asuint(rsqrt(asfloat(r2.x)));
	r2.xy = asuint(asfloat(r1.yz) * asfloat(r2.xx));
	r0.zw = asuint(asfloat(r2.xy) * float2(0.055, 0.055) + asfloat(r0.zw));
	{
		const float4 fetched = MotionVectorsTex.SampleLevel(MotionVectorsSampler, float2(asfloat(r0.z), asfloat(r0.w)), 0.0);
		r0.zw = asuint(fetched.xy);
	}
	r0.xy = asuint(cb0[3].xy * asfloat(r0.xy) + asfloat(r1.yz));
	r0.xy = asuint(asfloat(r0.xy) * cb0[3].zw);
	{
		const float4 fetched = AccumAOTex.SampleLevel(AccumAOSampler, float2(asfloat(r0.x), asfloat(r0.y)), 0.0);
		r2.xyz = asuint(fetched.xyz);
	}
	r0.x = asuint(dot(asfloat(r2.yz), float2(0.99610895, 0.0038910506)));
	r0.x = asuint(-asfloat(r0.x) + asfloat(r1.w));
	r0.x = asuint(-abs(asfloat(r0.x)) * 200.0 + 1.0);
	r0.yz = asuint(-asfloat(r0.zw) + asfloat(r1.yz));
	r0.y = asuint(dot(asfloat(r0.yz), asfloat(r0.yz)));
	r0.y = asuint(sqrt(asfloat(r0.y)));
	r0.y = asuint(-asfloat(r0.y) * 200.0 + 1.0);
	r0.xy = asuint(max(asfloat(r0.xy), float2(0.0, 0.2)));
	r0.x = asuint(asfloat(r0.x) * asfloat(r0.y));
	r0.x = asuint(asfloat(r0.x) * 0.99);
	r0.y = asuint(-asfloat(r1.x) + asfloat(r2.x));
	r0.x = asuint(asfloat(r0.x) * asfloat(r0.y) + asfloat(r1.x));
	r0.y = asuint(-(int)((asfloat(r1.x)) >= (0.95)));
	r0.z = asuint(-(int)((asfloat(r2.x)) < (0.7)));
	r0.y = r0.z & r0.y;
	r3.x = (r0.y != 0u ? r1.x : r0.x);
	r3.w = 0x3f800000u;
	g_OutputBuffer[uint2(dispatchID.x, dispatchID.y)] = float4(asfloat(r3.x), asfloat(r3.y), asfloat(r3.z), asfloat(r3.w));
}

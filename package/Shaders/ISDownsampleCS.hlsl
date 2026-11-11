// ISDownsampleCS and ISDownsampleIgnoreBrightestCS: the compute downsample of the image-based lens
// flare chain. PICK_BRIGHTEST keeps the brightest of each 2x2 block; GRIDSIZE_X is 1024 for both.
//
// Transcribed mechanically from the vanilla DXBC (Skyrim - Shaders.bsa), so its arithmetic, registers and
// constant layout are vanilla's. Temporaries hold raw bits (uint4), as DXBC registers do; integer work goes
// through asint/asuint and float work through asfloat. Checked against the vanilla shader on random inputs.

#if defined(PICK_BRIGHTEST)
SamplerState SourceSampler : register(s0);
SamplerState PreviousFrameSourceSampler : register(s1);
SamplerState MotionVectorsSampler : register(s2);
Texture2D<float4> SourceTex : register(t0);
Texture2D<float4> PreviousFrameSourceTex : register(t1);
Texture2D<float4> MotionVectorsTex : register(t2);
RWTexture2D<float4> g_OutputBuffer : register(u0);
groupshared uint g0[1024][4];

// The engine uploads this block whole (SetComputeShaderConstants); its constant list is
// TexelSize, SamplesCount, CompensateJittering, OffsetsAndWeights[16], g_DynamicResolutionInfos, DynamicResolutionInfosClamped.
cbuffer PerTechnique : register(b0)
{
	float4 cb0[19];
};

[numthreads(1024, 1, 1)] void main(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID) {
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0;
	r0.x = asuint(asint(groupID.x) * asint(0x00000400u) + asint(groupThreadID.x));
	r0.x = r0.x >> (0x00000002u & 31u);
	r1.x = asuint((float)(asint(r0.x)));
	r1.y = asuint((float)(asint(groupID.y)));
	r1.xy = asuint(asfloat(r1.xy) + float2(0.5, 0.5));
	r1.xy = asuint(asfloat(r1.xy) * cb0[0].xy);
	r1.zw = asuint(asfloat(r1.xy) * float2(4.0, 4.0));
	r2.xy = groupThreadID.xx & uint2(0x00000001u, 0x00000003u);
	r2.zw = asuint(cb0[asint(r2.y) + 1].xy * cb0[0].xy + asfloat(r1.zw));
	r2.zw = asuint(min(asfloat(r2.zw), cb0[18].xy));
	{
		const float4 fetched = SourceTex.SampleLevel(SourceSampler, float2(asfloat(r2.z), asfloat(r2.w)), 0.0);
		r3.xyz = asuint(fetched.xyz);
	}
	r3.w = asuint(dot(asfloat(r3.xyz), float3(0.3, 0.59, 0.11)));
	g0[groupThreadID.x][0] = r3.x;
	g0[groupThreadID.x][1] = r3.y;
	g0[groupThreadID.x][2] = r3.z;
	g0[groupThreadID.x][3] = r3.w;
	GroupMemoryBarrierWithGroupSync();
	r2.y = asuint(-(int)((asint(r2.y)) == (asint(0x00000000u))));
	if (r2.x == 0u) {
		r2.x = asuint(asint(groupThreadID.x) + asint(0x00000001u));
		r2.z = g0[r2.x][3];
		r2.z = asuint(-(int)((asfloat(r2.z)) < (asfloat(r3.w))));
		r4.xyzw = uint4(g0[r2.x][0], g0[r2.x][1], g0[r2.x][2], g0[r2.x][3]);
		r3.xyzw = uint4((r2.z != 0u ? r3.x : r4.x), (r2.z != 0u ? r3.y : r4.y), (r2.z != 0u ? r3.z : r4.z), (r2.z != 0u ? r3.w : r4.w));
		g0[groupThreadID.x][0] = r3.x;
		g0[groupThreadID.x][1] = r3.y;
		g0[groupThreadID.x][2] = r3.z;
		g0[groupThreadID.x][3] = r3.w;
	}
	GroupMemoryBarrierWithGroupSync();
	r2.xz = asuint(-(int2)((asfloat(r1.zw)) < (cb0[17].xy)));
	r2.x = r2.x & r2.y;
	r2.x = r2.z & r2.x;
	if (r2.x != 0u) {
		r2.xyzw = uint4(g0[groupThreadID.x][0], g0[groupThreadID.x][1], g0[groupThreadID.x][2], g0[groupThreadID.x][3]);
		r3.x = asuint(asint(groupThreadID.x) + asint(0x00000002u));
		r3.xyzw = uint4(g0[r3.x][0], g0[r3.x][1], g0[r3.x][2], g0[r3.x][3]);
		r2.w = asuint(-(int)((asfloat(r3.w)) < (asfloat(r2.w))));
		r2.xyz = uint3((r2.w != 0u ? r2.x : r3.x), (r2.w != 0u ? r2.y : r3.y), (r2.w != 0u ? r2.z : r3.z));
		if (asuint(cb0[0].w) != 0u) {
			{
				const float4 fetched = MotionVectorsTex.SampleLevel(MotionVectorsSampler, float2(asfloat(r1.z), asfloat(r1.w)), 0.0);
				r1.zw = asuint(fetched.xy);
			}
			r1.xy = asuint(asfloat(r1.xy) * float2(4.0, 4.0) + asfloat(r1.zw));
			r1.zw = asuint(-cb0[0].xy + cb0[18].zw);
			r1.xy = asuint(min(asfloat(r1.zw), asfloat(r1.xy)));
			{
				const float4 fetched = PreviousFrameSourceTex.SampleLevel(PreviousFrameSourceSampler, float2(asfloat(r1.x), asfloat(r1.y)), 0.0);
				r1.xyz = asuint(fetched.xyz);
			}
			r1.xyz = asuint(asfloat(r1.xyz) + asfloat(r2.xyz));
			r2.xyz = asuint(asfloat(r1.xyz) * float3(0.5, 0.5, 0.5));
		}
		r0.yzw = groupID.yyy;
		g_OutputBuffer[uint2(r0.x, r0.y)] = float4(asfloat(r2.x), asfloat(r2.y), asfloat(r2.z), asfloat(r2.x));
	}
}
#else
SamplerState SourceSampler : register(s0);
SamplerState PreviousFrameSourceSampler : register(s1);
SamplerState MotionVectorsSampler : register(s2);
Texture2D<float4> SourceTex : register(t0);
Texture2D<float4> PreviousFrameSourceTex : register(t1);
Texture2D<float4> MotionVectorsTex : register(t2);
RWTexture2D<float4> g_OutputBuffer : register(u0);
groupshared uint g0[1024][4];

// The engine uploads this block whole (SetComputeShaderConstants); its constant list is
// TexelSize, SamplesCount, CompensateJittering, OffsetsAndWeights[16], g_DynamicResolutionInfos, DynamicResolutionInfosClamped.
cbuffer PerTechnique : register(b0)
{
	float4 cb0[19];
};

[numthreads(1024, 1, 1)] void main(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID) {
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0;
	r0.x = asuint(asint(groupID.x) * asint(0x00000400u) + asint(groupThreadID.x));
	r0.x = r0.x >> (0x00000002u & 31u);
	r1.x = asuint((float)(asint(r0.x)));
	r1.y = asuint((float)(asint(groupID.y)));
	r1.xy = asuint(asfloat(r1.xy) + float2(0.5, 0.5));
	r1.xy = asuint(asfloat(r1.xy) * cb0[0].xy);
	r1.zw = asuint(asfloat(r1.xy) * float2(4.0, 4.0));
	r2.x = groupThreadID.x & 0x00000003u;
	r2.yz = asuint(cb0[asint(r2.x) + 1].xy * cb0[0].xy + asfloat(r1.zw));
	r2.yz = asuint(min(asfloat(r2.yz), cb0[18].xy));
	{
		const float4 fetched = SourceTex.SampleLevel(SourceSampler, float2(asfloat(r2.y), asfloat(r2.z)), 0.0);
		r2.yzw = asuint(fetched.xyz);
	}
	g0[groupThreadID.x][0] = r2.y;
	g0[groupThreadID.x][1] = r2.z;
	g0[groupThreadID.x][2] = r2.w;
	GroupMemoryBarrierWithGroupSync();
	r2.x = asuint(-(int)((asint(r2.x)) == (asint(0x00000000u))));
	r3.xy = asuint(-(int2)((asfloat(r1.zw)) < (cb0[17].xy)));
	r2.x = r2.x & r3.x;
	r2.x = r3.y & r2.x;
	if (r2.x != 0u) {
		r3.xyz = asuint(asint(groupThreadID.xxx) + int3(asint(0x00000001u), asint(0x00000002u), asint(0x00000003u)));
		r4.xyz = uint3(g0[r3.x][0], g0[r3.x][1], g0[r3.x][2]);
		r2.xyz = asuint(asfloat(r2.yzw) + asfloat(r4.xyz));
		r3.xyw = uint3(g0[r3.y][0], g0[r3.y][1], g0[r3.y][2]);
		r2.xyz = asuint(asfloat(r2.xyz) + asfloat(r3.xyw));
		r3.xyz = uint3(g0[r3.z][0], g0[r3.z][1], g0[r3.z][2]);
		r2.xyz = asuint(asfloat(r2.xyz) + asfloat(r3.xyz));
		r3.xyz = asuint(asfloat(r2.xyz) * float3(0.25, 0.25, 0.25));
		if (asuint(cb0[0].w) != 0u) {
			{
				const float4 fetched = MotionVectorsTex.SampleLevel(MotionVectorsSampler, float2(asfloat(r1.z), asfloat(r1.w)), 0.0);
				r1.zw = asuint(fetched.xy);
			}
			r1.xy = asuint(asfloat(r1.xy) * float2(4.0, 4.0) + asfloat(r1.zw));
			r1.zw = asuint(-cb0[0].xy + cb0[18].zw);
			r1.xy = asuint(min(asfloat(r1.zw), asfloat(r1.xy)));
			{
				const float4 fetched = PreviousFrameSourceTex.SampleLevel(PreviousFrameSourceSampler, float2(asfloat(r1.x), asfloat(r1.y)), 0.0);
				r1.xyz = asuint(fetched.xyz);
			}
			r1.xyz = asuint(asfloat(r2.xyz) * float3(0.25, 0.25, 0.25) + asfloat(r1.xyz));
			r3.xyz = asuint(asfloat(r1.xyz) * float3(0.5, 0.5, 0.5));
		}
		r0.yzw = groupID.yyy;
		g_OutputBuffer[uint2(r0.x, r0.y)] = float4(asfloat(r3.x), asfloat(r3.y), asfloat(r3.z), asfloat(r3.x));
	}
}
#endif

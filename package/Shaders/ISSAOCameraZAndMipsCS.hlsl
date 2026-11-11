// ISSAOCameraZAndMipsCS: linear camera depth and its first mips for the compute SAO.
//
// Transcribed mechanically from the vanilla DXBC (Skyrim - Shaders.bsa), so its arithmetic, registers and
// constant layout are vanilla's. Temporaries hold raw bits (uint4), as DXBC registers do; integer work goes
// through asint/asuint and float work through asfloat. Checked against the vanilla shader on random inputs.

Texture2D<float4> DepthTex : register(t0);
RWTexture2D<float4> g_DepthMip0 : register(u0);
RWTexture2D<float4> g_DepthMip1 : register(u1);
RWTexture2D<float4> g_DepthMip2 : register(u2);
groupshared uint g0[64][1];

// The engine uploads this block whole (SetComputeShaderConstants); its constant list is
// g_ClipInfos.
cbuffer PerTechnique : register(b0)
{
	float4 cb0[1];
};

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID, uint3 groupThreadID : SV_GroupThreadID) {
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0;
	r0.xyzw = asuint((float4)(dispatchID.xyyy));
	r1.xy = asuint(asfloat(r0.xw) + asfloat(r0.xw));
	r1.xy = (uint2)(asfloat(r1.xy));
	r2.xyzw = asuint(asint(r1.xyxy) + int4(asint(0x00000000u), asint(0x00000001u), asint(0x00000001u), asint(0x00000000u)));
	r3.xy = asuint(asint(r1.xy) + int2(asint(0x00000001u), asint(0x00000001u)));
	r1.zw = uint2(0x00000000u, 0x00000000u);
	{
		const float4 fetched = DepthTex.Load(int3(asint(r1.x), asint(r1.y), asint(r1.w)));
		r1.z = asuint(fetched.x);
	}
	r1.z = asuint(cb0[0].y * asfloat(r1.z) + cb0[0].z);
	r1.z = asuint(cb0[0].x / asfloat(r1.z));
	r4.xy = r2.zw;
	r4.zw = uint2(0x00000000u, 0x00000000u);
	{
		const float4 fetched = DepthTex.Load(int3(asint(r4.x), asint(r4.y), asint(r4.w)));
		r1.w = asuint(fetched.x);
	}
	r1.w = asuint(cb0[0].y * asfloat(r1.w) + cb0[0].z);
	r1.w = asuint(cb0[0].x / asfloat(r1.w));
	r2.zw = uint2(0x00000000u, 0x00000000u);
	{
		const float4 fetched = DepthTex.Load(int3(asint(r2.x), asint(r2.y), asint(r2.w)));
		r2.z = asuint(fetched.x);
	}
	r2.z = asuint(cb0[0].y * asfloat(r2.z) + cb0[0].z);
	r2.z = asuint(cb0[0].x / asfloat(r2.z));
	r3.zw = uint2(0x00000000u, 0x00000000u);
	{
		const float4 fetched = DepthTex.Load(int3(asint(r3.x), asint(r3.y), asint(r3.w)));
		r2.w = asuint(fetched.x);
	}
	r2.w = asuint(cb0[0].y * asfloat(r2.w) + cb0[0].z);
	r2.w = asuint(cb0[0].x / asfloat(r2.w));
	r3.z = asuint(min(asfloat(r1.w), asfloat(r1.z)));
	r3.w = asuint(min(asfloat(r2.w), asfloat(r2.z)));
	r3.z = asuint(min(asfloat(r3.w), asfloat(r3.z)));
	g_DepthMip0[uint2(r1.x, r1.y)] = float4(asfloat(r1.z), asfloat(r1.z), asfloat(r1.z), asfloat(r1.z));
	g_DepthMip0[uint2(r4.x, r4.y)] = float4(asfloat(r1.w), asfloat(r1.w), asfloat(r1.w), asfloat(r1.w));
	g_DepthMip0[uint2(r2.x, r2.y)] = float4(asfloat(r2.z), asfloat(r2.z), asfloat(r2.z), asfloat(r2.z));
	g_DepthMip0[uint2(r3.x, r3.y)] = float4(asfloat(r2.w), asfloat(r2.w), asfloat(r2.w), asfloat(r2.w));
	r1.x = asuint(asint(groupThreadID.y) * asint(0x00000008u) + asint(groupThreadID.x));
	g0[r1.x][0] = r3.z;
	GroupMemoryBarrierWithGroupSync();
	g_DepthMip1[uint2(dispatchID.x, dispatchID.y)] = float4(asfloat(r3.z), asfloat(r3.z), asfloat(r3.z), asfloat(r3.z));
	r1.yz = dispatchID.xy & uint2(0x00000001u, 0x00000001u);
	r1.yz = asuint(-(int2)((asint(r1.yz)) == (int2(asint(0x00000000u), asint(0x00000000u)))));
	r1.y = r1.z & r1.y;
	if (r1.y != 0u) {
		r1.xyz = asuint(asint(r1.xxx) + int3(asint(0x00000001u), asint(0x00000008u), asint(0x00000009u)));
		r1.x = g0[r1.x][0];
		r1.y = g0[r1.y][0];
		r1.z = g0[r1.z][0];
		r1.x = asuint(min(asfloat(r1.x), asfloat(r3.z)));
		r1.y = asuint(min(asfloat(r1.z), asfloat(r1.y)));
		r1.x = asuint(min(asfloat(r1.y), asfloat(r1.x)));
		r0.xyzw = asuint(asfloat(r0.xyzw) * float4(0.5, 0.5, 0.5, 0.5));
		r0.xyzw = (uint4)(asfloat(r0.xyzw));
		g_DepthMip2[uint2(r0.x, r0.y)] = float4(asfloat(r1.x), asfloat(r1.x), asfloat(r1.x), asfloat(r1.x));
	}
}

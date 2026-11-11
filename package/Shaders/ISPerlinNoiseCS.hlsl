// ISPerlinNoiseCS and ISPerlinNoise2DCS: simplex noise into a 3D texture, or with TEXTURE2D a 2D one.
//
// Transcribed mechanically from the vanilla DXBC (Skyrim - Shaders.bsa), so its arithmetic, registers and
// constant layout are vanilla's. Temporaries hold raw bits (uint4), as DXBC registers do; integer work goes
// through asint/asuint and float work through asfloat. Checked against the vanilla shader on random inputs.

#if defined(TEXTURE2D)
RWTexture2D<float4> Output0 : register(u0);

// The engine uploads this block whole (SetComputeShaderConstants); its constant list is
// g_Tex3DSize, OutDensityTexture.
cbuffer PerTechnique : register(b0)
{
	float4 cb0[1];
};

[numthreads(32, 32, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0;
	r0.xw = uint2(0x00000000u, 0x3f800000u);
	r1.xyz = asuint((float3)(dispatchID.xyz));
	r1.xyz = asuint(asfloat(r1.xyz) * float3(0.05, 0.05, 0.05));
	r1.xyz = asuint(asfloat(r1.xyz) / cb0[0].xyz);
	r2.xyz = asuint(asfloat(r1.xyz) * float3(160.0, 160.0, 160.0));
	r1.w = asuint(dot(asfloat(r2.xyz), float3(0.33333334, 0.33333334, 0.33333334)));
	r2.xyz = asuint(asfloat(r1.xyz) * float3(160.0, 160.0, 160.0) + asfloat(r1.www));
	r2.xyz = asuint(floor(asfloat(r2.xyz)));
	r3.xyz = asuint(asfloat(r2.xyz) * float3(0.0034602077, 0.0034602077, 0.0034602077));
	r3.xyz = asuint(floor(asfloat(r3.xyz)));
	r3.xyz = asuint(-asfloat(r3.xyz) * float3(289.0, 289.0, 289.0) + asfloat(r2.xyz));
	r1.xyz = asuint(asfloat(r1.xyz) * float3(160.0, 160.0, 160.0) + -asfloat(r2.xyz));
	r1.w = asuint(dot(asfloat(r2.xyz), float3(0.16666667, 0.16666667, 0.16666667)));
	r1.xyz = asuint(asfloat(r1.www) + asfloat(r1.xyz));
	r2.xyz = asuint(-(int3)((asfloat(r1.zxy)) >= (asfloat(r1.xyz))));
	r4.xyz = r2.yzx & uint3(0x3f800000u, 0x3f800000u, 0x3f800000u);
	r2.xyz = uint3((r2.x != 0u ? 0x00000000u : 0x3f800000u), (r2.y != 0u ? 0x00000000u : 0x3f800000u), (r2.z != 0u ? 0x00000000u : 0x3f800000u));
	r5.xyz = asuint(min(asfloat(r2.xyz), asfloat(r4.xyz)));
	r2.xyz = asuint(max(asfloat(r2.xyz), asfloat(r4.xyz)));
	r0.y = r5.z;
	r0.z = r2.z;
	r0.xyzw = asuint(asfloat(r0.xyzw) + asfloat(r3.zzzz));
	r4.xyzw = asuint(asfloat(r0.xyzw) * float4(34.0, 34.0, 34.0, 34.0) + float4(1.0, 1.0, 1.0, 1.0));
	r0.xyzw = asuint(asfloat(r0.xyzw) * asfloat(r4.xyzw));
	r4.xyzw = asuint(asfloat(r0.xyzw) * float4(0.0034602077, 0.0034602077, 0.0034602077, 0.0034602077));
	r4.xyzw = asuint(floor(asfloat(r4.xyzw)));
	r0.xyzw = asuint(-asfloat(r4.xyzw) * float4(289.0, 289.0, 289.0, 289.0) + asfloat(r0.xyzw));
	r0.xyzw = asuint(asfloat(r3.yyyy) + asfloat(r0.xyzw));
	r4.xw = uint2(0x00000000u, 0x3f800000u);
	r4.y = r5.y;
	r4.z = r2.y;
	r0.xyzw = asuint(asfloat(r0.xyzw) + asfloat(r4.xyzw));
	r4.xyzw = asuint(asfloat(r0.xyzw) * float4(34.0, 34.0, 34.0, 34.0) + float4(1.0, 1.0, 1.0, 1.0));
	r0.xyzw = asuint(asfloat(r0.xyzw) * asfloat(r4.xyzw));
	r4.xyzw = asuint(asfloat(r0.xyzw) * float4(0.0034602077, 0.0034602077, 0.0034602077, 0.0034602077));
	r4.xyzw = asuint(floor(asfloat(r4.xyzw)));
	r0.xyzw = asuint(-asfloat(r4.xyzw) * float4(289.0, 289.0, 289.0, 289.0) + asfloat(r0.xyzw));
	r0.xyzw = asuint(asfloat(r3.xxxx) + asfloat(r0.xyzw));
	r3.xw = uint2(0x00000000u, 0x3f800000u);
	r3.y = r5.x;
	r4.xyz = asuint(asfloat(r1.xyz) + -asfloat(r5.xyz));
	r4.xyz = asuint(asfloat(r4.xyz) + float3(0.16666667, 0.16666667, 0.16666667));
	r3.z = r2.x;
	r2.xyz = asuint(asfloat(r1.xyz) + -asfloat(r2.xyz));
	r2.xyz = asuint(asfloat(r2.xyz) + float3(0.33333334, 0.33333334, 0.33333334));
	r0.xyzw = asuint(asfloat(r0.xyzw) + asfloat(r3.xyzw));
	r3.xyzw = asuint(asfloat(r0.xyzw) * float4(34.0, 34.0, 34.0, 34.0) + float4(1.0, 1.0, 1.0, 1.0));
	r0.xyzw = asuint(asfloat(r0.xyzw) * asfloat(r3.xyzw));
	r3.xyzw = asuint(asfloat(r0.xyzw) * float4(0.0034602077, 0.0034602077, 0.0034602077, 0.0034602077));
	r3.xyzw = asuint(floor(asfloat(r3.xyzw)));
	r0.xyzw = asuint(-asfloat(r3.xyzw) * float4(289.0, 289.0, 289.0, 289.0) + asfloat(r0.xyzw));
	r3.xyzw = asuint(asfloat(r0.xyzw) * float4(0.020408165, 0.020408165, 0.020408165, 0.020408165));
	r3.xyzw = asuint(floor(asfloat(r3.xyzw)));
	r0.xyzw = asuint(-asfloat(r3.xyzw) * float4(49.0, 49.0, 49.0, 49.0) + asfloat(r0.xyzw));
	r3.xyzw = asuint(asfloat(r0.xyzw) * float4(0.14285715, 0.14285715, 0.14285715, 0.14285715));
	r3.xyzw = asuint(floor(asfloat(r3.xyzw)));
	r0.xyzw = asuint(-asfloat(r3.xyzw) * float4(7.0, 7.0, 7.0, 7.0) + asfloat(r0.xyzw));
	r3.xyzw = asuint(asfloat(r3.zxwy) * float4(0.2857143, 0.2857143, 0.2857143, 0.2857143) + float4(-0.9285714, -0.9285714, -0.9285714, -0.9285714));
	r0.xyzw = asuint(asfloat(r0.xyzw) * float4(0.2857143, 0.2857143, 0.2857143, 0.2857143) + float4(-0.9285714, -0.9285714, -0.9285714, -0.9285714));
	r5.yw = r0.xy;
	r5.xz = r3.yw;
	r6.yw = asuint(floor(asfloat(r0.xy)));
	r6.xz = asuint(floor(asfloat(r3.yw)));
	r6.xyzw = asuint(asfloat(r6.xyzw) * float4(2.0, 2.0, 2.0, 2.0) + float4(1.0, 1.0, 1.0, 1.0));
	r7.xyzw = asuint(-abs(asfloat(r3.ywxz)) + float4(1.0, 1.0, 1.0, 1.0));
	r7.xyzw = asuint(-abs(asfloat(r0.xywz)) + asfloat(r7.xywz));
	r8.xyzw = asuint(-(int4)((float4(0.0, 0.0, 0.0, 0.0)) >= (asfloat(r7.xywz))));
	r8.xyzw = uint4((r8.x != 0u ? 0xbf800000u : 0x80000000u), (r8.y != 0u ? 0xbf800000u : 0x80000000u), (r8.z != 0u ? 0xbf800000u : 0x80000000u), (r8.w != 0u ? 0xbf800000u : 0x80000000u));
	r5.xyzw = asuint(asfloat(r6.zwxy) * asfloat(r8.yyxx) + asfloat(r5.zwxy));
	r6.xy = r5.zw;
	r6.z = r7.x;
	r9.x = asuint(dot(asfloat(r6.xyz), asfloat(r6.xyz)));
	r5.z = r7.y;
	r9.y = asuint(dot(asfloat(r5.xyz), asfloat(r5.xyz)));
	r10.yw = asuint(floor(asfloat(r0.zw)));
	r3.yw = r0.zw;
	r10.xz = asuint(floor(asfloat(r3.xz)));
	r0.xyzw = asuint(asfloat(r10.xyzw) * float4(2.0, 2.0, 2.0, 2.0) + float4(1.0, 1.0, 1.0, 1.0));
	r0.xyzw = asuint(asfloat(r0.xyzw) * asfloat(r8.zzww) + asfloat(r3.xyzw));
	r7.xy = r0.zw;
	r0.z = r7.w;
	r9.z = asuint(dot(asfloat(r0.xyz), asfloat(r0.xyz)));
	r9.w = asuint(dot(asfloat(r7.xyz), asfloat(r7.xyz)));
	r3.xyzw = asuint(-asfloat(r9.xyzw) * float4(0.85373473, 0.85373473, 0.85373473, 0.85373473) + float4(1.7928429, 1.7928429, 1.7928429, 1.7928429));
	r5.xyz = asuint(asfloat(r3.yyy) * asfloat(r5.xyz));
	r5.y = asuint(dot(asfloat(r5.xyz), asfloat(r4.xyz)));
	r4.y = asuint(dot(asfloat(r4.xyz), asfloat(r4.xyz)));
	r0.xyz = asuint(asfloat(r0.xyz) * asfloat(r3.zzz));
	r5.z = asuint(dot(asfloat(r0.xyz), asfloat(r2.xyz)));
	r4.z = asuint(dot(asfloat(r2.xyz), asfloat(r2.xyz)));
	r0.xyz = asuint(asfloat(r3.xxx) * asfloat(r6.xyz));
	r2.xyz = asuint(asfloat(r3.www) * asfloat(r7.xyz));
	r5.x = asuint(dot(asfloat(r0.xyz), asfloat(r1.xyz)));
	r0.xyz = asuint(asfloat(r1.xyz) + float3(-0.5, -0.5, -0.5));
	r4.x = asuint(dot(asfloat(r1.xyz), asfloat(r1.xyz)));
	r5.w = asuint(dot(asfloat(r2.xyz), asfloat(r0.xyz)));
	r4.w = asuint(dot(asfloat(r0.xyz), asfloat(r0.xyz)));
	r0.xyzw = asuint(-asfloat(r4.xyzw) + float4(0.6, 0.6, 0.6, 0.6));
	r0.xyzw = asuint(max(asfloat(r0.xyzw), float4(0.0, 0.0, 0.0, 0.0)));
	r0.xyzw = asuint(asfloat(r0.xyzw) * asfloat(r0.xyzw));
	r0.xyzw = asuint(asfloat(r0.xyzw) * asfloat(r0.xyzw));
	r0.x = asuint(dot(asfloat(r0.xyzw), asfloat(r5.xyzw)));
	r0.x = asuint(asfloat(r0.x) * 21.0 + 0.5);
	Output0[uint2(dispatchID.x, dispatchID.y)] = float4(asfloat(r0.x), asfloat(r0.x), asfloat(r0.x), asfloat(r0.x));
}
#else
RWTexture3D<float4> Output0 : register(u0);

// The engine uploads this block whole (SetComputeShaderConstants); its constant list is
// g_Tex3DSize, OutDensityTexture.
cbuffer PerTechnique : register(b0)
{
	float4 cb0[1];
};

[numthreads(32, 32, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0;
	r0.xw = uint2(0x00000000u, 0x3f800000u);
	r1.xyz = asuint((float3)(dispatchID.xyz));
	r1.xyz = asuint(asfloat(r1.xyz) + float3(0.2, 0.0, 0.0));
	r1.xyz = asuint(asfloat(r1.xyz) / cb0[0].xyz);
	r2.xyz = asuint(asfloat(r1.xyz) * float3(80.0, 80.0, 80.0));
	r1.w = asuint(dot(asfloat(r2.xyz), float3(0.33333334, 0.33333334, 0.33333334)));
	r2.xyz = asuint(asfloat(r1.xyz) * float3(80.0, 80.0, 80.0) + asfloat(r1.www));
	r2.xyz = asuint(floor(asfloat(r2.xyz)));
	r3.xyz = asuint(asfloat(r2.xyz) * float3(0.0034602077, 0.0034602077, 0.0034602077));
	r3.xyz = asuint(floor(asfloat(r3.xyz)));
	r3.xyz = asuint(-asfloat(r3.xyz) * float3(289.0, 289.0, 289.0) + asfloat(r2.xyz));
	r1.xyz = asuint(asfloat(r1.xyz) * float3(80.0, 80.0, 80.0) + -asfloat(r2.xyz));
	r1.w = asuint(dot(asfloat(r2.xyz), float3(0.16666667, 0.16666667, 0.16666667)));
	r1.xyz = asuint(asfloat(r1.www) + asfloat(r1.xyz));
	r2.xyz = asuint(-(int3)((asfloat(r1.zxy)) >= (asfloat(r1.xyz))));
	r4.xyz = r2.yzx & uint3(0x3f800000u, 0x3f800000u, 0x3f800000u);
	r2.xyz = uint3((r2.x != 0u ? 0x00000000u : 0x3f800000u), (r2.y != 0u ? 0x00000000u : 0x3f800000u), (r2.z != 0u ? 0x00000000u : 0x3f800000u));
	r5.xyz = asuint(min(asfloat(r2.xyz), asfloat(r4.xyz)));
	r2.xyz = asuint(max(asfloat(r2.xyz), asfloat(r4.xyz)));
	r0.y = r5.z;
	r0.z = r2.z;
	r0.xyzw = asuint(asfloat(r0.xyzw) + asfloat(r3.zzzz));
	r4.xyzw = asuint(asfloat(r0.xyzw) * float4(34.0, 34.0, 34.0, 34.0) + float4(1.0, 1.0, 1.0, 1.0));
	r0.xyzw = asuint(asfloat(r0.xyzw) * asfloat(r4.xyzw));
	r4.xyzw = asuint(asfloat(r0.xyzw) * float4(0.0034602077, 0.0034602077, 0.0034602077, 0.0034602077));
	r4.xyzw = asuint(floor(asfloat(r4.xyzw)));
	r0.xyzw = asuint(-asfloat(r4.xyzw) * float4(289.0, 289.0, 289.0, 289.0) + asfloat(r0.xyzw));
	r0.xyzw = asuint(asfloat(r3.yyyy) + asfloat(r0.xyzw));
	r4.xw = uint2(0x00000000u, 0x3f800000u);
	r4.y = r5.y;
	r4.z = r2.y;
	r0.xyzw = asuint(asfloat(r0.xyzw) + asfloat(r4.xyzw));
	r4.xyzw = asuint(asfloat(r0.xyzw) * float4(34.0, 34.0, 34.0, 34.0) + float4(1.0, 1.0, 1.0, 1.0));
	r0.xyzw = asuint(asfloat(r0.xyzw) * asfloat(r4.xyzw));
	r4.xyzw = asuint(asfloat(r0.xyzw) * float4(0.0034602077, 0.0034602077, 0.0034602077, 0.0034602077));
	r4.xyzw = asuint(floor(asfloat(r4.xyzw)));
	r0.xyzw = asuint(-asfloat(r4.xyzw) * float4(289.0, 289.0, 289.0, 289.0) + asfloat(r0.xyzw));
	r0.xyzw = asuint(asfloat(r3.xxxx) + asfloat(r0.xyzw));
	r3.xw = uint2(0x00000000u, 0x3f800000u);
	r3.y = r5.x;
	r4.xyz = asuint(asfloat(r1.xyz) + -asfloat(r5.xyz));
	r4.xyz = asuint(asfloat(r4.xyz) + float3(0.16666667, 0.16666667, 0.16666667));
	r3.z = r2.x;
	r2.xyz = asuint(asfloat(r1.xyz) + -asfloat(r2.xyz));
	r2.xyz = asuint(asfloat(r2.xyz) + float3(0.33333334, 0.33333334, 0.33333334));
	r0.xyzw = asuint(asfloat(r0.xyzw) + asfloat(r3.xyzw));
	r3.xyzw = asuint(asfloat(r0.xyzw) * float4(34.0, 34.0, 34.0, 34.0) + float4(1.0, 1.0, 1.0, 1.0));
	r0.xyzw = asuint(asfloat(r0.xyzw) * asfloat(r3.xyzw));
	r3.xyzw = asuint(asfloat(r0.xyzw) * float4(0.0034602077, 0.0034602077, 0.0034602077, 0.0034602077));
	r3.xyzw = asuint(floor(asfloat(r3.xyzw)));
	r0.xyzw = asuint(-asfloat(r3.xyzw) * float4(289.0, 289.0, 289.0, 289.0) + asfloat(r0.xyzw));
	r3.xyzw = asuint(asfloat(r0.xyzw) * float4(0.020408165, 0.020408165, 0.020408165, 0.020408165));
	r3.xyzw = asuint(floor(asfloat(r3.xyzw)));
	r0.xyzw = asuint(-asfloat(r3.xyzw) * float4(49.0, 49.0, 49.0, 49.0) + asfloat(r0.xyzw));
	r3.xyzw = asuint(asfloat(r0.xyzw) * float4(0.14285715, 0.14285715, 0.14285715, 0.14285715));
	r3.xyzw = asuint(floor(asfloat(r3.xyzw)));
	r0.xyzw = asuint(-asfloat(r3.xyzw) * float4(7.0, 7.0, 7.0, 7.0) + asfloat(r0.xyzw));
	r3.xyzw = asuint(asfloat(r3.zxwy) * float4(0.2857143, 0.2857143, 0.2857143, 0.2857143) + float4(-0.9285714, -0.9285714, -0.9285714, -0.9285714));
	r0.xyzw = asuint(asfloat(r0.xyzw) * float4(0.2857143, 0.2857143, 0.2857143, 0.2857143) + float4(-0.9285714, -0.9285714, -0.9285714, -0.9285714));
	r5.yw = r0.xy;
	r5.xz = r3.yw;
	r6.yw = asuint(floor(asfloat(r0.xy)));
	r6.xz = asuint(floor(asfloat(r3.yw)));
	r6.xyzw = asuint(asfloat(r6.xyzw) * float4(2.0, 2.0, 2.0, 2.0) + float4(1.0, 1.0, 1.0, 1.0));
	r7.xyzw = asuint(-abs(asfloat(r3.ywxz)) + float4(1.0, 1.0, 1.0, 1.0));
	r7.xyzw = asuint(-abs(asfloat(r0.xywz)) + asfloat(r7.xywz));
	r8.xyzw = asuint(-(int4)((float4(0.0, 0.0, 0.0, 0.0)) >= (asfloat(r7.xywz))));
	r8.xyzw = uint4((r8.x != 0u ? 0xbf800000u : 0x80000000u), (r8.y != 0u ? 0xbf800000u : 0x80000000u), (r8.z != 0u ? 0xbf800000u : 0x80000000u), (r8.w != 0u ? 0xbf800000u : 0x80000000u));
	r5.xyzw = asuint(asfloat(r6.zwxy) * asfloat(r8.yyxx) + asfloat(r5.zwxy));
	r6.xy = r5.zw;
	r6.z = r7.x;
	r9.x = asuint(dot(asfloat(r6.xyz), asfloat(r6.xyz)));
	r5.z = r7.y;
	r9.y = asuint(dot(asfloat(r5.xyz), asfloat(r5.xyz)));
	r10.yw = asuint(floor(asfloat(r0.zw)));
	r3.yw = r0.zw;
	r10.xz = asuint(floor(asfloat(r3.xz)));
	r0.xyzw = asuint(asfloat(r10.xyzw) * float4(2.0, 2.0, 2.0, 2.0) + float4(1.0, 1.0, 1.0, 1.0));
	r0.xyzw = asuint(asfloat(r0.xyzw) * asfloat(r8.zzww) + asfloat(r3.xyzw));
	r7.xy = r0.zw;
	r0.z = r7.w;
	r9.z = asuint(dot(asfloat(r0.xyz), asfloat(r0.xyz)));
	r9.w = asuint(dot(asfloat(r7.xyz), asfloat(r7.xyz)));
	r3.xyzw = asuint(-asfloat(r9.xyzw) * float4(0.85373473, 0.85373473, 0.85373473, 0.85373473) + float4(1.7928429, 1.7928429, 1.7928429, 1.7928429));
	r5.xyz = asuint(asfloat(r3.yyy) * asfloat(r5.xyz));
	r5.y = asuint(dot(asfloat(r5.xyz), asfloat(r4.xyz)));
	r4.y = asuint(dot(asfloat(r4.xyz), asfloat(r4.xyz)));
	r0.xyz = asuint(asfloat(r0.xyz) * asfloat(r3.zzz));
	r5.z = asuint(dot(asfloat(r0.xyz), asfloat(r2.xyz)));
	r4.z = asuint(dot(asfloat(r2.xyz), asfloat(r2.xyz)));
	r0.xyz = asuint(asfloat(r3.xxx) * asfloat(r6.xyz));
	r2.xyz = asuint(asfloat(r3.www) * asfloat(r7.xyz));
	r5.x = asuint(dot(asfloat(r0.xyz), asfloat(r1.xyz)));
	r0.xyz = asuint(asfloat(r1.xyz) + float3(-0.5, -0.5, -0.5));
	r4.x = asuint(dot(asfloat(r1.xyz), asfloat(r1.xyz)));
	r5.w = asuint(dot(asfloat(r2.xyz), asfloat(r0.xyz)));
	r4.w = asuint(dot(asfloat(r0.xyz), asfloat(r0.xyz)));
	r0.xyzw = asuint(-asfloat(r4.xyzw) + float4(0.6, 0.6, 0.6, 0.6));
	r0.xyzw = asuint(max(asfloat(r0.xyzw), float4(0.0, 0.0, 0.0, 0.0)));
	r0.xyzw = asuint(asfloat(r0.xyzw) * asfloat(r0.xyzw));
	r0.xyzw = asuint(asfloat(r0.xyzw) * asfloat(r0.xyzw));
	r0.x = asuint(dot(asfloat(r0.xyzw), asfloat(r5.xyzw)));
	r0.x = asuint(asfloat(r0.x) * 21.0 + 0.5);
	Output0[uint3(dispatchID.x, dispatchID.y, dispatchID.z)] = float4(asfloat(r0.x), asfloat(r0.x), asfloat(r0.x), asfloat(r0.x));
}
#endif

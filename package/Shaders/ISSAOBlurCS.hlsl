// ISSAOBlurHCS and ISSAOBlurVCS: the depth-aware compute blur of the SAO term. AXIS_V is the vertical
// pass; GRID_SIZE is 972 horizontally and 552 vertically.
//
// Transcribed mechanically from the vanilla DXBC (Skyrim - Shaders.bsa), so its arithmetic, registers and
// constant layout are vanilla's. Temporaries hold raw bits (uint4), as DXBC registers do; integer work goes
// through asint/asuint and float work through asfloat. Checked against the vanilla shader on random inputs.

#if defined(CS_ORG_BINDLESS)
// The ORG build (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0 and u0 from the heap, and the constants the engine uploaded
// for this dispatch (CS b0) from the engine-constants capture through the pass's latch. Both passes have one list.
#	define ORG_SHARED_CONSTANTS
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISSAOBLURCS_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, sourceTex, 0) X(UAV, RWTexture2D<float4>, g_OutputBuffer, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 0)
ORG_PUSH_BLOCK(ISSAOBLURCS_ORG_RESOURCES(ORG_HANDLE))
ISSAOBLURCS_ORG_RESOURCES(ORG_RESOURCE)
// g_ScreenInfos, as the engine uploaded it.
static float4 cb0[1] = { OrgEngineRow(PerGeometryAt, 0) };
#endif

// The halo lanes read past the image's edges, and the last group writes past them.
#include "Common/OrgTexelBounds.hlsli"

#if defined(AXIS_V)
#	if !defined(CS_ORG_BINDLESS)
Texture2D<float4> sourceTex : register(t0);
RWTexture2D<float4> g_OutputBuffer : register(u0);

// The engine uploads this block whole (SetComputeShaderConstants); its constant list is
// g_ScreenInfos.
cbuffer PerTechnique : register(b0)
{
	float4 cb0[1];
};
#	endif
groupshared uint g0[552][2];

[numthreads(1, 552, 1)] void main(uint3 dispatchID : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID) {
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0;
	r0.x = asuint(asint(groupThreadID.y) + asint(0xfffffffau));
	r0.y = (uint)(cb0[0].y);
	r1.y = asuint(asint(groupID.y) * asint(0x0000021cu) + asint(r0.x));
	r1.x = groupID.x;
	r1.zw = uint2(0x00000000u, 0x00000000u);
	{
		const float4 fetched = ORG_LOAD_OR_ZERO(sourceTex, int3(asint(r1.x), asint(r1.y), asint(r1.w)));
		r2.xyz = asuint(fetched.xyz);
	}
	r2.y = asuint(dot(asfloat(r2.yz), float2(0.996109, 0.00389105)));
	g0[groupThreadID.y][0] = r2.x;
	g0[groupThreadID.y][1] = r2.y;
	GroupMemoryBarrierWithGroupSync();
	r0.z = asuint(-(int)((asint(r0.x)) >= (asint(0x00000000u))));
	r0.w = asuint(-(int)((asint(r0.x)) < (asint(0x0000021cu))));
	r0.z = r0.w & r0.z;
	r0.y = asuint(-(int)((r1.y) < (r0.y)));
	r0.y = r0.y & r0.z;
	if (r0.y != 0u) {
		r0.y = asuint(-(int)((asfloat(r2.y)) == (1.0)));
		if (r0.y != 0u) {
			ORG_STORE_INSIDE(g_OutputBuffer, uint2(dispatchID.x, dispatchID.y), float4(0.0, 0.0, 0.0, 0.0));
			return;
		}
		r0.xy = uint2(g0[r0.x][0], g0[r0.x][1]);
		r0.y = asuint(-asfloat(r2.y) + asfloat(r0.y));
		r0.y = asuint(-abs(asfloat(r0.y)) * 2000.0 + 1.0);
		r0.y = asuint(max(asfloat(r0.y), 0.0));
		r0.z = asuint(asfloat(r0.y) * 0.39290202);
		r0.x = asuint(asfloat(r0.z) * asfloat(r0.x));
		r0.x = asuint(asfloat(r2.x) * 0.15317 + asfloat(r0.x));
		r0.y = asuint(asfloat(r0.y) * 0.39290202 + 0.15317);
		r3.xyzw = asuint(asint(groupThreadID.yyyy) + int4(asint(0xfffffffcu), asint(0xfffffffeu), asint(0x00000002u), asint(0x00000004u)));
		r0.zw = uint2(g0[r3.x][0], g0[r3.x][1]);
		r0.w = asuint(-asfloat(r2.y) + asfloat(r0.w));
		r0.w = asuint(-abs(asfloat(r0.w)) * 2000.0 + 1.0);
		r0.w = asuint(max(asfloat(r0.w), 0.0));
		r1.z = asuint(asfloat(r0.w) * 0.42264903);
		r0.x = asuint(asfloat(r0.z) * asfloat(r1.z) + asfloat(r0.x));
		r0.y = asuint(asfloat(r0.w) * 0.42264903 + asfloat(r0.y));
		r0.zw = uint2(g0[r3.y][0], g0[r3.y][1]);
		r0.w = asuint(-asfloat(r2.y) + asfloat(r0.w));
		r0.w = asuint(-abs(asfloat(r0.w)) * 2000.0 + 1.0);
		r0.w = asuint(max(asfloat(r0.w), 0.0));
		r1.z = asuint(asfloat(r0.w) * 0.444893);
		r0.x = asuint(asfloat(r0.z) * asfloat(r1.z) + asfloat(r0.x));
		r0.y = asuint(asfloat(r0.w) * 0.444893 + asfloat(r0.y));
		r0.zw = uint2(g0[r3.z][0], g0[r3.z][1]);
		r0.w = asuint(-asfloat(r2.y) + asfloat(r0.w));
		r0.w = asuint(-abs(asfloat(r0.w)) * 2000.0 + 1.0);
		r0.w = asuint(max(asfloat(r0.w), 0.0));
		r1.z = asuint(asfloat(r0.w) * 0.444893);
		r0.x = asuint(asfloat(r0.z) * asfloat(r1.z) + asfloat(r0.x));
		r0.y = asuint(asfloat(r0.w) * 0.444893 + asfloat(r0.y));
		r0.zw = uint2(g0[r3.w][0], g0[r3.w][1]);
		r0.w = asuint(-asfloat(r2.y) + asfloat(r0.w));
		r0.w = asuint(-abs(asfloat(r0.w)) * 2000.0 + 1.0);
		r0.w = asuint(max(asfloat(r0.w), 0.0));
		r1.z = asuint(asfloat(r0.w) * 0.42264903);
		r0.x = asuint(asfloat(r0.z) * asfloat(r1.z) + asfloat(r0.x));
		r0.y = asuint(asfloat(r0.w) * 0.42264903 + asfloat(r0.y));
		r0.z = asuint(asint(groupThreadID.y) + asint(0x00000006u));
		r0.zw = uint2(g0[r0.z][0], g0[r0.z][1]);
		r0.w = asuint(-asfloat(r2.y) + asfloat(r0.w));
		r0.w = asuint(-abs(asfloat(r0.w)) * 2000.0 + 1.0);
		r0.w = asuint(max(asfloat(r0.w), 0.0));
		r1.z = asuint(asfloat(r0.w) * 0.39290202);
		r0.x = asuint(asfloat(r0.z) * asfloat(r1.z) + asfloat(r0.x));
		r0.y = asuint(asfloat(r0.w) * 0.39290202 + asfloat(r0.y));
		r0.y = asuint(asfloat(r0.y) + 0.0001);
		r0.x = asuint(asfloat(r0.x) / asfloat(r0.y));
		ORG_STORE_INSIDE(g_OutputBuffer, uint2(r1.x, r1.y), float4(asfloat(r0.x), asfloat(r0.x), asfloat(r0.x), asfloat(r0.x)));
	}
}
#else
#	if !defined(CS_ORG_BINDLESS)
Texture2D<float4> sourceTex : register(t0);
RWTexture2D<float4> g_OutputBuffer : register(u0);

// The engine uploads this block whole (SetComputeShaderConstants); its constant list is
// g_ScreenInfos.
cbuffer PerTechnique : register(b0)
{
	float4 cb0[1];
};
#	endif
groupshared uint g0[972][2];

[numthreads(972, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID) {
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0;
	r0.x = asuint(asint(groupThreadID.x) + asint(0xfffffffau));
	r0.y = (uint)(cb0[0].x);
	r1.x = asuint(asint(groupID.x) * asint(0x000003c0u) + asint(r0.x));
	r1.y = groupID.y;
	r1.zw = uint2(0x00000000u, 0x00000000u);
	{
		const float4 fetched = ORG_LOAD_OR_ZERO(sourceTex, int3(asint(r1.x), asint(r1.y), asint(r1.w)));
		r2.xyz = asuint(fetched.xyz);
	}
	r3.y = asuint(dot(asfloat(r2.yz), float2(0.996109, 0.00389105)));
	r3.x = r2.x;
	g0[groupThreadID.x][0] = r3.x;
	g0[groupThreadID.x][1] = r3.y;
	GroupMemoryBarrierWithGroupSync();
	r0.z = asuint(-(int)((asint(r0.x)) >= (asint(0x00000000u))));
	r0.w = asuint(-(int)((asint(r0.x)) < (asint(0x000003c0u))));
	r0.z = r0.w & r0.z;
	r0.y = asuint(-(int)((r1.x) < (r0.y)));
	r0.y = r0.y & r0.z;
	if (r0.y != 0u) {
		r0.y = asuint(-(int)((asfloat(r3.y)) == (1.0)));
		if (r0.y != 0u) {
			ORG_STORE_INSIDE(g_OutputBuffer, uint2(dispatchID.x, dispatchID.y), float4(0.0, 0.0, 0.0, 0.0));
			return;
		}
		r0.xy = uint2(g0[r0.x][0], g0[r0.x][1]);
		r0.y = asuint(-asfloat(r3.y) + asfloat(r0.y));
		r0.y = asuint(-abs(asfloat(r0.y)) * 2000.0 + 1.0);
		r0.y = asuint(max(asfloat(r0.y), 0.0));
		r0.z = asuint(asfloat(r0.y) * 0.39290202);
		r0.x = asuint(asfloat(r0.z) * asfloat(r0.x));
		r0.x = asuint(asfloat(r3.x) * 0.15317 + asfloat(r0.x));
		r0.y = asuint(asfloat(r0.y) * 0.39290202 + 0.15317);
		r4.xyzw = asuint(asint(groupThreadID.xxxx) + int4(asint(0xfffffffcu), asint(0xfffffffeu), asint(0x00000002u), asint(0x00000004u)));
		r0.zw = uint2(g0[r4.x][0], g0[r4.x][1]);
		r0.w = asuint(-asfloat(r3.y) + asfloat(r0.w));
		r0.w = asuint(-abs(asfloat(r0.w)) * 2000.0 + 1.0);
		r0.w = asuint(max(asfloat(r0.w), 0.0));
		r1.z = asuint(asfloat(r0.w) * 0.42264903);
		r0.x = asuint(asfloat(r0.z) * asfloat(r1.z) + asfloat(r0.x));
		r0.y = asuint(asfloat(r0.w) * 0.42264903 + asfloat(r0.y));
		r0.zw = uint2(g0[r4.y][0], g0[r4.y][1]);
		r0.w = asuint(-asfloat(r3.y) + asfloat(r0.w));
		r0.w = asuint(-abs(asfloat(r0.w)) * 2000.0 + 1.0);
		r0.w = asuint(max(asfloat(r0.w), 0.0));
		r1.z = asuint(asfloat(r0.w) * 0.444893);
		r0.x = asuint(asfloat(r0.z) * asfloat(r1.z) + asfloat(r0.x));
		r0.y = asuint(asfloat(r0.w) * 0.444893 + asfloat(r0.y));
		r0.zw = uint2(g0[r4.z][0], g0[r4.z][1]);
		r0.w = asuint(-asfloat(r3.y) + asfloat(r0.w));
		r0.w = asuint(-abs(asfloat(r0.w)) * 2000.0 + 1.0);
		r0.w = asuint(max(asfloat(r0.w), 0.0));
		r1.z = asuint(asfloat(r0.w) * 0.444893);
		r0.x = asuint(asfloat(r0.z) * asfloat(r1.z) + asfloat(r0.x));
		r0.y = asuint(asfloat(r0.w) * 0.444893 + asfloat(r0.y));
		r0.zw = uint2(g0[r4.w][0], g0[r4.w][1]);
		r0.w = asuint(-asfloat(r3.y) + asfloat(r0.w));
		r0.w = asuint(-abs(asfloat(r0.w)) * 2000.0 + 1.0);
		r0.w = asuint(max(asfloat(r0.w), 0.0));
		r1.z = asuint(asfloat(r0.w) * 0.42264903);
		r0.x = asuint(asfloat(r0.z) * asfloat(r1.z) + asfloat(r0.x));
		r0.y = asuint(asfloat(r0.w) * 0.42264903 + asfloat(r0.y));
		r0.z = asuint(asint(groupThreadID.x) + asint(0x00000006u));
		r0.zw = uint2(g0[r0.z][0], g0[r0.z][1]);
		r0.w = asuint(-asfloat(r3.y) + asfloat(r0.w));
		r0.w = asuint(-abs(asfloat(r0.w)) * 2000.0 + 1.0);
		r0.w = asuint(max(asfloat(r0.w), 0.0));
		r1.z = asuint(asfloat(r0.w) * 0.39290202);
		r0.x = asuint(asfloat(r0.z) * asfloat(r1.z) + asfloat(r0.x));
		r0.y = asuint(asfloat(r0.w) * 0.39290202 + asfloat(r0.y));
		r0.y = asuint(asfloat(r0.y) + 0.0001);
		r2.x = asuint(asfloat(r0.x) / asfloat(r0.y));
		r2.w = 0x00000000u;
		ORG_STORE_INSIDE(g_OutputBuffer, uint2(r1.x, r1.y), float4(asfloat(r2.x), asfloat(r2.y), asfloat(r2.z), asfloat(r2.w)));
	}
}
#endif

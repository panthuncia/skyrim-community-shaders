// ISTemporalAA_UI (TAA_UI) and ISTemporalAA_Water (TAA_BLEND_WATER): vanilla's two TAA forms that CS's ISTemporalAA.hlsl
// does not rewrite. Included by ISTemporalAA.hlsl, whose vertex stage they share.
//
// Transcribed from the vanilla DXBC (shadersfx/shaders011.fxp, the groups after ISSnowSSS) one instruction per line. Temporaries
// are uint4 raw bits, as DXBC registers are; float work goes through asfloat. Literals are hex so each keeps every bit.
//
// The engine uploads PerGeometry as one 0x60-byte block (BSImagespaceShaderISTemporalAA_UI::sub, AE 0x14151ff70), not by name,
// so its rows are fixed: c0 InverseSize_MaxUV, c1 UVOffset_SampleWeight8_Zero, c2 SampleWeights0123, c3 SampleWeights4567,
// c4 LowFrequency_HighFrequency_Sharpen_Overlay, c5 NearFar_Menu_EffectThreshold. Their values come from AE FUN_1414d9580
// (UI) and FUN_1414d9a80 (water): c0 inverse size and the dynamic-resolution UV limit, c1 the jitter, c2 the four neighbour
// weights, c3 one texel's UV step, c4 the blend weights, c5 the near and far planes, the menu flag and the mask threshold.
//
// Slots: t0 source, t1 history, t2 motion vectors, t3 depth, t4 the TAA mask. The water form adds t5, the normals target whose z
// marks water, and t6, the water history. The UI form writes the image and its history, the water form also the water history.

#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): the slots and their samplers from the heap,
// the shared constants, and PerGeometry from the engine-constants capture through the pass's latch. The vertex stage is the
// graph's fullscreen triangle.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	if defined(TAA_BLEND_WATER)
#		define ISTEMPORALAA_FORM_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, source, 0) X(SRV, Texture2D<float4>, accumulationBuffer, 1) X(SRV, Texture2D<float4>, motionBuffer, 2) X(SRV, Texture2D<float4>, depthBuffer, 3) X(SRV, Texture2D<float4>, effectMask, 4) X(SRV, Texture2D<float4>, waterMask, 5) X(SRV, Texture2D<float4>, waterHistoryBuffer, 6) X(SAMPLER, SamplerState, sourceSampler, 0) X(SAMPLER, SamplerState, accumulationSampler, 1) X(SAMPLER, SamplerState, motionSampler, 2) X(SAMPLER, SamplerState, depthSampler, 3) X(SAMPLER, SamplerState, effectMaskSampler, 4) X(SAMPLER, SamplerState, waterMaskSampler, 5) X(SAMPLER, SamplerState, waterHistorySampler, 6) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
#	else
#		define ISTEMPORALAA_FORM_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, source, 0) X(SRV, Texture2D<float4>, accumulationBuffer, 1) X(SRV, Texture2D<float4>, motionBuffer, 2) X(SRV, Texture2D<float4>, depthBuffer, 3) X(SRV, Texture2D<float4>, effectMask, 4) X(SAMPLER, SamplerState, sourceSampler, 0) X(SAMPLER, SamplerState, accumulationSampler, 1) X(SAMPLER, SamplerState, motionSampler, 2) X(SAMPLER, SamplerState, depthSampler, 3) X(SAMPLER, SamplerState, effectMaskSampler, 4) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
#	endif
ORG_PUSH_BLOCK(ISTEMPORALAA_FORM_ORG_RESOURCES(ORG_HANDLE))
ISTEMPORALAA_FORM_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/FrameBuffer.hlsli"

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
// PerGeometry by row, as the D3D11 cbuffer below places it.
static float4 InverseSize_MaxUV = OrgEngineRow(PerGeometryAt, 0);
static float4 UVOffset_SampleWeight8_Zero = OrgEngineRow(PerGeometryAt, 1);
static float4 SampleWeights0123 = OrgEngineRow(PerGeometryAt, 2);
static float4 SampleWeights4567 = OrgEngineRow(PerGeometryAt, 3);
static float4 LowFrequency_HighFrequency_Sharpen_Overlay = OrgEngineRow(PerGeometryAt, 4);
static float4 NearFar_Menu_EffectThreshold = OrgEngineRow(PerGeometryAt, 5);
#	else
Texture2D<float4> source : register(t0);
Texture2D<float4> accumulationBuffer : register(t1);
Texture2D<float4> motionBuffer : register(t2);
Texture2D<float4> depthBuffer : register(t3);
Texture2D<float4> effectMask : register(t4);

SamplerState sourceSampler : register(s0);
SamplerState accumulationSampler : register(s1);
SamplerState motionSampler : register(s2);
SamplerState depthSampler : register(s3);
SamplerState effectMaskSampler : register(s4);

#		if defined(TAA_BLEND_WATER)
Texture2D<float4> waterMask : register(t5);
Texture2D<float4> waterHistoryBuffer : register(t6);

SamplerState waterMaskSampler : register(s5);
SamplerState waterHistorySampler : register(s6);
#		endif

cbuffer PerGeometry : register(b2)
{
	float4 InverseSize_MaxUV : packoffset(c0);
	float4 UVOffset_SampleWeight8_Zero : packoffset(c1);
	float4 SampleWeights0123 : packoffset(c2);
	float4 SampleWeights4567 : packoffset(c3);
	float4 LowFrequency_HighFrequency_Sharpen_Overlay : packoffset(c4);
	float4 NearFar_Menu_EffectThreshold : packoffset(c5);
};
#	endif

#	define TAA_T0 source
#	define TAA_T1 accumulationBuffer
#	define TAA_T2 motionBuffer
#	define TAA_T3 depthBuffer
#	define TAA_T4 effectMask
#	define TAA_T5 waterMask
#	define TAA_T6 waterHistoryBuffer
#	define TAA_S0 sourceSampler
#	define TAA_S1 accumulationSampler
#	define TAA_S2 motionSampler
#	define TAA_S3 depthSampler
#	define TAA_S4 effectMaskSampler
#	define TAA_S5 waterMaskSampler
#	define TAA_S6 waterHistorySampler
#	define TAA_DR1 FrameBuffer::DynamicResolutionParams1
#	define TAA_DR2 FrameBuffer::DynamicResolutionParams2

struct TAA_FORM_OUTPUT
{
	float4 Color: SV_Target0;
	float4 History: SV_Target1;
#	if defined(TAA_BLEND_WATER)
	float4 WaterHistory: SV_Target2;
#	endif
};

TAA_FORM_OUTPUT main(float4 position: SV_POSITION0, float2 texCoord: TEXCOORD0)
{
	float4 v1 = float4(texCoord, 0, 0);
	uint4 o0 = 0, o1 = 0, o2 = 0;
#	if defined(TAA_BLEND_WATER)
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0, r16 = 0, r17 = 0;
	r0.xy = asuint((v1.xy + -SampleWeights4567.xy));
	r0.zw = asuint((v1.xy + SampleWeights4567.xy));
	r1.xy = asuint((asfloat(r0.zw) * TAA_DR1.xy));
	r1.xy = asuint(max(asfloat(r1.xy), float2(asfloat(0x00000000u), asfloat(0x00000000u))));
	r2.x = asuint(TAA_DR2.z);
	r2.y = asuint(TAA_DR1.y);
	r1.xy = asuint(min(asfloat(r1.xy), asfloat(r2.xy)));
	r1.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r1.xy)).x);
	r3.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r1.xy)).yxz);
	r4.xyzw = asuint((SampleWeights4567.xyxy * float4(asfloat(0x3f800000u), asfloat(0xbf800000u), asfloat(0x3f800000u), asfloat(0x00000000u)) + v1.xyxy));
	r5.xyzw = asuint((asfloat(r4.xyzw) * TAA_DR1.xyxy));
	r5.xyzw = asuint(max(asfloat(r5.xyzw), float4(asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u))));
	r5.xyzw = asuint(min(asfloat(r2.xyxy), asfloat(r5.xyzw)));
	r1.x = asuint(TAA_T3.Sample(TAA_S3, asfloat(r5.xy)).x);
	r1.y = asuint(min(asfloat(r1.z), asfloat(r1.x)));
	r1.zw = asuint((asfloat(r0.xy) * TAA_DR1.xy));
	r1.zw = asuint(max(asfloat(r1.zw), float2(asfloat(0x00000000u), asfloat(0x00000000u))));
	r1.zw = asuint(min(asfloat(r2.xy), asfloat(r1.zw)));
	r2.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r1.zw)).x);
	r6.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r1.zw)).yxz);
	r1.y = asuint(min(asfloat(r1.y), asfloat(r2.z)));
	r1.z = asuint(-(int)(asfloat(r1.y) == asfloat(r2.z)));
	r0.xy = (r1.zz != 0u ? r0.xy : r0.zw);
	r0.z = asuint(-(int)(asfloat(r1.y) == asfloat(r1.x)));
	r0.xy = (r0.zz != 0u ? r4.xy : r0.xy);
	r7.xyzw = asuint((SampleWeights4567.xyxy * float4(asfloat(0x00000000u), asfloat(0xbf800000u), asfloat(0xbf800000u), asfloat(0x3f800000u)) + v1.xyxy));
	r8.xyzw = asuint((asfloat(r7.xyzw) * TAA_DR1.xyxy));
	r8.xyzw = asuint(max(asfloat(r8.xyzw), float4(asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u))));
	r8.xyzw = asuint(min(asfloat(r2.xyxy), asfloat(r8.xyzw)));
	r0.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r8.xy)).x);
	r0.w = asuint(min(asfloat(r1.y), asfloat(r0.z)));
	r1.x = asuint(TAA_T3.Sample(TAA_S3, asfloat(r5.zw)).x);
	r0.w = asuint(min(asfloat(r0.w), asfloat(r1.x)));
	r1.x = asuint(-(int)(asfloat(r0.w) == asfloat(r1.x)));
	r0.xy = (r1.xx != 0u ? r4.zw : r0.xy);
	r0.z = asuint(-(int)(asfloat(r0.w) == asfloat(r0.z)));
	r0.xy = (r0.zz != 0u ? r7.xy : r0.xy);
	r1.xyzw = asuint((SampleWeights4567.xyxy * float4(asfloat(0xbf800000u), asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x3f800000u)) + v1.xyxy));
	r4.xyzw = asuint((asfloat(r1.xyzw) * TAA_DR1.xyxy));
	r4.xyzw = asuint(max(asfloat(r4.xyzw), float4(asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u))));
	r4.xyzw = asuint(min(asfloat(r2.xyxy), asfloat(r4.xyzw)));
	r0.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r4.xy)).x);
	r0.w = asuint(min(asfloat(r0.w), asfloat(r0.z)));
	r2.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r8.zw)).x);
	r0.w = asuint(min(asfloat(r0.w), asfloat(r2.z)));
	r2.z = asuint(-(int)(asfloat(r0.w) == asfloat(r2.z)));
	r0.xy = (r2.zz != 0u ? r7.zw : r0.xy);
	r0.z = asuint(-(int)(asfloat(r0.w) == asfloat(r0.z)));
	r0.xy = (r0.zz != 0u ? r1.xy : r0.xy);
	r1.xy = asuint((v1.xy * TAA_DR1.xy));
	r1.xy = asuint(max(asfloat(r1.xy), float2(asfloat(0x00000000u), asfloat(0x00000000u))));
	r1.xy = asuint(min(asfloat(r2.xy), asfloat(r1.xy)));
	r0.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r1.xy)).x);
	r0.w = asuint(min(asfloat(r0.w), asfloat(r0.z)));
	r2.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r4.zw)).x);
	r0.w = asuint(min(asfloat(r0.w), asfloat(r2.z)));
	r2.z = asuint(-(int)(asfloat(r0.w) == asfloat(r2.z)));
	r0.xy = (r2.zz != 0u ? r1.zw : r0.xy);
	r0.z = asuint(-(int)(asfloat(r0.w) == asfloat(r0.z)));
	r0.w = asuint((asfloat(r0.w) * asfloat(0x40000000u) + asfloat(0xbf800000u)));
	r0.xy = (r0.zz != 0u ? asuint(v1.xy) : r0.xy);
	r0.xy = asuint((asfloat(r0.xy) * TAA_DR1.xy));
	r0.xy = asuint(max(asfloat(r0.xy), float2(asfloat(0x00000000u), asfloat(0x00000000u))));
	r0.xy = asuint(min(asfloat(r2.xy), asfloat(r0.xy)));
	r0.xy = asuint(TAA_T2.Sample(TAA_S2, asfloat(r0.xy)).xy);
	r1.zw = asuint((asfloat(r0.xy) + v1.xy));
	r0.x = asuint(dot(asfloat(r0.xy), asfloat(r0.xy)));
	r0.x = asuint(sqrt(asfloat(r0.x)));
	r7.xyzw = asuint((asfloat(r1.zwzw) * TAA_DR1.zwxy));
	r7.xyzw = asuint(max(asfloat(r7.xyzw), float4(asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u))));
	r9.x = asuint(min(asfloat(r7.x), TAA_DR2.w));
	r9.y = asuint(min(asfloat(r7.y), TAA_DR1.w));
	r0.yz = asuint(min(asfloat(r2.xy), asfloat(r7.zw)));
	r2.xyzw = asuint(TAA_T6.Sample(TAA_S6, asfloat(r0.yz)).xyzw);
	r7.xyz = asuint(TAA_T1.Sample(TAA_S1, asfloat(r9.xy)).xzy);
	r3.w = asuint(dot(asfloat(r3.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r0.y = asuint(-(int)(asfloat(r3.w) < asfloat(r7.x)));
	r6.w = asuint(dot(asfloat(r6.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r0.z = asuint(-(int)(asfloat(r6.w) < asfloat(r7.x)));
	r9.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r5.xy)).yxz);
	r5.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r5.zw)).yxz);
	r9.w = asuint(dot(asfloat(r9.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r3.x = asuint(-(int)(asfloat(r9.w) < asfloat(r7.x)));
	r5.w = asuint(dot(asfloat(r5.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r5.x = asuint(-(int)(asfloat(r5.w) < asfloat(r7.x)));
	r10.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r8.xy)).yxz);
	r8.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r8.zw)).yxz);
	r10.w = asuint(dot(asfloat(r10.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r6.x = asuint(-(int)(asfloat(r10.w) < asfloat(r7.x)));
	r8.w = asuint(dot(asfloat(r8.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r9.x = asuint(-(int)(asfloat(r8.w) < asfloat(r7.x)));
	r11.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r4.xy)).yxz);
	r4.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r4.zw)).yxz);
	r11.w = asuint(dot(asfloat(r11.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r10.x = asuint(-(int)(asfloat(r11.w) < asfloat(r7.x)));
	r4.w = asuint(dot(asfloat(r4.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r12.x = asuint(-(int)(asfloat(r4.w) < asfloat(r7.x)));
	r12.yzw = asuint(TAA_T0.Sample(TAA_S0, asfloat(r1.xy)).xyz);
	r13.x = asuint(dot(asfloat(r12.zwy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r14.x = asuint(-(int)(asfloat(r13.x) < asfloat(r7.x)));
	r13.yz = r12.yw;
	r14.y = asuint(-(int)(asfloat(r13.x) < asfloat(0x3f8020c5u)));
	r14.yzw = (r14.yyy != 0u ? r13.yzx : uint3(0x3f8020c5u, 0x3f8020c5u, 0x3f8020c5u));
	r14.yzw = (r14.xxx != 0u ? uint3(0x3f8020c5u, 0x3f8020c5u, 0x3f8020c5u) : r14.yzw);
	r15.x = asuint(-(int)(asfloat(r4.w) < asfloat(r14.w)));
	r15.xyz = (r15.xxx != 0u ? r4.yzw : r14.yzw);
	r14.yzw = (r12.xxx != 0u ? r14.yzw : r15.xyz);
	r15.xyz = asuint((asfloat(r11.yxz) * SampleWeights0123.zzz));
	r15.xyz = asuint((asfloat(r8.yxz) * SampleWeights0123.www + asfloat(r15.xyz)));
	r15.xyz = asuint((asfloat(r4.yxz) * SampleWeights0123.yyy + asfloat(r15.xyz)));
	r15.xyz = asuint((asfloat(r12.yzw) * SampleWeights0123.xxx + asfloat(r15.xyz)));
	r4.x = asuint(-(int)(asfloat(r11.w) < asfloat(r14.w)));
	r16.xyz = (r4.xxx != 0u ? r11.yzw : r14.yzw);
	r14.yzw = (r10.xxx != 0u ? r14.yzw : r16.xyz);
	r4.x = asuint(-(int)(asfloat(r8.w) < asfloat(r14.w)));
	r16.xyz = (r4.xxx != 0u ? r8.yzw : r14.yzw);
	r14.yzw = (r9.xxx != 0u ? r14.yzw : r16.xyz);
	r4.x = asuint(-(int)(asfloat(r10.w) < asfloat(r14.w)));
	r16.xyz = (r4.xxx != 0u ? r10.yzw : r14.yzw);
	r14.yzw = (r6.xxx != 0u ? r14.yzw : r16.xyz);
	r4.x = asuint(-(int)(asfloat(r5.w) < asfloat(r14.w)));
	r16.xyz = (r4.xxx != 0u ? r5.yzw : r14.yzw);
	r14.yzw = (r5.xxx != 0u ? r14.yzw : r16.xyz);
	r4.x = asuint(-(int)(asfloat(r9.w) < asfloat(r14.w)));
	r16.xyz = (r4.xxx != 0u ? r9.yzw : r14.yzw);
	r14.yzw = (r3.xxx != 0u ? r14.yzw : r16.xyz);
	r4.x = asuint(-(int)(asfloat(r6.w) < asfloat(r14.w)));
	r16.xyz = (r4.xxx != 0u ? r6.yzw : r14.yzw);
	r16.yzw = (r0.zzz != 0u ? r14.yzw : r16.xyz);
	r4.x = asuint(-(int)(asfloat(r3.w) < asfloat(r16.w)));
	r17.yzw = (r4.xxx != 0u ? r3.yzw : r16.yzw);
	r4.x = asuint(-(int)(asfloat(0xba83126fu) < asfloat(r13.x)));
	r14.yzw = (r4.xxx != 0u ? r13.yzx : uint3(0xba83126fu, 0xba83126fu, 0xba83126fu));
	r14.xyz = (r14.xxx != 0u ? r14.yzw : uint3(0xba83126fu, 0xba83126fu, 0xba83126fu));
	r4.x = asuint(-(int)(asfloat(r14.z) < asfloat(r4.w)));
	r4.xyz = (r4.xxx != 0u ? r4.yzw : r14.xyz);
	r4.xyz = (r12.xxx != 0u ? r4.xyz : r14.xyz);
	r4.w = asuint((-asfloat(r4.w) + asfloat(r13.x)));
	r4.w = asuint((-abs(asfloat(r4.w)) + asfloat(0x3e4ccccdu)));
	r4.w = asuint(ceil(asfloat(r4.w)));
	r8.x = asuint(-(int)(asfloat(r4.z) < asfloat(r11.w)));
	r11.xyz = (r8.xxx != 0u ? r11.yzw : r4.xyz);
	r4.xyz = (r10.xxx != 0u ? r11.xyz : r4.xyz);
	r8.x = asuint((-asfloat(r11.w) + asfloat(r13.x)));
	r8.x = asuint((-abs(asfloat(r8.x)) + asfloat(0x3e4ccccdu)));
	r10.x = asuint(-(int)(asfloat(r4.z) < asfloat(r8.w)));
	r11.xyz = (r10.xxx != 0u ? r8.yzw : r4.xyz);
	r4.xyz = (r9.xxx != 0u ? r11.xyz : r4.xyz);
	r8.y = asuint((-asfloat(r8.w) + asfloat(r13.x)));
	r8.y = asuint((-abs(asfloat(r8.y)) + asfloat(0x3e4ccccdu)));
	r8.xy = asuint(ceil(asfloat(r8.xy)));
	r8.z = asuint(-(int)(asfloat(r4.z) < asfloat(r10.w)));
	r10.xyz = (r8.zzz != 0u ? r10.yzw : r4.xyz);
	r4.xyz = (r6.xxx != 0u ? r10.xyz : r4.xyz);
	r6.x = asuint((-asfloat(r10.w) + asfloat(r13.x)));
	r6.x = asuint((-abs(asfloat(r6.x)) + asfloat(0x3e4ccccdu)));
	r6.x = asuint(ceil(asfloat(r6.x)));
	r8.z = asuint(-(int)(asfloat(r4.z) < asfloat(r5.w)));
	r10.xyz = (r8.zzz != 0u ? r5.yzw : r4.xyz);
	r4.xyz = (r5.xxx != 0u ? r10.xyz : r4.xyz);
	r5.x = asuint((-asfloat(r5.w) + asfloat(r13.x)));
	r5.x = asuint((-abs(asfloat(r5.x)) + asfloat(0x3e4ccccdu)));
	r5.x = asuint(ceil(asfloat(r5.x)));
	r5.y = asuint(-(int)(asfloat(r4.z) < asfloat(r9.w)));
	r5.yzw = (r5.yyy != 0u ? r9.yzw : r4.xyz);
	r4.xyz = (r3.xxx != 0u ? r5.yzw : r4.xyz);
	r3.x = asuint((-asfloat(r9.w) + asfloat(r13.x)));
	r3.x = asuint((-abs(asfloat(r3.x)) + asfloat(0x3e4ccccdu)));
	r3.x = asuint(ceil(asfloat(r3.x)));
	r5.y = asuint(-(int)(asfloat(r4.z) < asfloat(r6.w)));
	r5.yzw = (r5.yyy != 0u ? r6.yzw : r4.xyz);
	r4.xyz = (r0.zzz != 0u ? r5.yzw : r4.xyz);
	r0.z = asuint((-asfloat(r6.w) + asfloat(r13.x)));
	r0.z = asuint((-abs(asfloat(r0.z)) + asfloat(0x3e4ccccdu)));
	r17.x = r4.y;
	r5.y = asuint(-(int)(asfloat(r4.z) < asfloat(r3.w)));
	r5.yzw = (r5.yyy != 0u ? r3.yzw : r4.xyz);
	r9.xw = (r0.yy != 0u ? r5.yw : r4.xz);
	r16.x = r5.z;
	r10.xyzw = (r0.yyyy != 0u ? r16.xyzw : r17.xyzw);
	r0.y = asuint((-asfloat(r3.w) + asfloat(r13.x)));
	r0.y = asuint((-abs(asfloat(r0.y)) + asfloat(0x3e4ccccdu)));
	r0.yz = asuint(ceil(asfloat(r0.yz)));
	r0.y = asuint((-asfloat(r0.y) + asfloat(0x40800000u)));
	r0.y = asuint((-asfloat(r0.z) + asfloat(r0.y)));
	r0.y = asuint((-asfloat(r3.x) + asfloat(r0.y)));
	r0.y = asuint((-asfloat(r5.x) + asfloat(r0.y)));
	r0.y = asuint((-asfloat(r6.x) + asfloat(r0.y)));
	r0.y = asuint((-asfloat(r8.y) + asfloat(r0.y)));
	r0.y = asuint((-asfloat(r8.x) + asfloat(r0.y)));
	r0.y = asuint(saturate((-asfloat(r4.w) + asfloat(r0.y))));
	r0.z = asuint(-(int)(asfloat(0x3f800000u) < asfloat(r10.w)));
	r3.x = asuint((-asfloat(r10.y) * asfloat(0x3e800000u) + asfloat(r10.w)));
	r3.x = asuint((-asfloat(r10.z) * asfloat(0x3e800000u) + asfloat(r3.x)));
	r3.y = asuint((asfloat(r3.x) + asfloat(r3.x)));
	r9.z = r10.x;
	r3.xzw = r10.yzw;
	r4.x = asuint((-asfloat(r9.x) * asfloat(0x3e800000u) + asfloat(r9.w)));
	r4.x = asuint((-asfloat(r10.x) * asfloat(0x3e800000u) + asfloat(r4.x)));
	r9.y = asuint((asfloat(r4.x) + asfloat(r4.x)));
	r4.x = asuint(-(int)(asfloat(r9.w) < asfloat(0x00000000u)));
	r4.xyzw = (r4.xxxx != 0u ? r3.xyzw : r9.xyzw);
	r5.xyzw = (r0.zzzz != 0u ? r4.xyzw : r3.xyzw);
	r0.z = asuint(max(asfloat(r4.w), asfloat(r7.x)));
	r6.x = asuint(min(asfloat(r5.w), asfloat(r0.z)));
	r6.z = r5.w;
	r6.y = r4.w;
	r8.z = r3.w;
	r8.x = r7.x;
	r8.y = r9.w;
	r0.z = asuint((asfloat(r7.z) * asfloat(0x3f733333u)));
	r0.y = asuint(saturate((asfloat(r0.y) * asfloat(0x3e800000u) + asfloat(r0.z))));
	r0.z = asuint(-(int)(asfloat(r0.y) < asfloat(0x3f670a3du)));
	r6.xyz = (r0.zzz != 0u ? r6.xyz : r8.xyz);
	r6.yz = asuint((-asfloat(r6.yy) + asfloat(r6.zx)));
	r7.x = r6.x;
	r3.w = asuint(-(int)(asfloat(0x3c23d70au) < asfloat(r6.y)));
	r4.w = asuint((asfloat(r6.z) / asfloat(r6.y)));
	r3.w = (r3.w != 0u ? r4.w : 0x3f000000u);
	r4.xyz = (r0.zzz != 0u ? r4.xyz : r9.xyz);
	r3.xyz = (r0.zzz != 0u ? r5.xyz : r3.xyz);
	r3.xyz = asuint((-asfloat(r4.xyz) + asfloat(r3.xyz)));
	r3.xyz = asuint((asfloat(r3.www) * asfloat(r3.xyz) + asfloat(r4.xyz)));
	r0.z = asuint(min(asfloat(r1.w), asfloat(r1.z)));
	r1.zw = asuint(-(int2)(asfloat(r1.zw) >= float2(asfloat(0x3f800000u), asfloat(0x3f800000u))));
	r0.z = asuint(-(int)(asfloat(0x00000000u) >= asfloat(r0.z)));
	r0.z = (r1.z | r0.z);
	r0.z = (r1.w | r0.z);
	r1.zw = asuint(TAA_T4.Sample(TAA_S4, asfloat(r1.xy)).xy);
	r1.x = asuint(TAA_T5.Sample(TAA_S5, asfloat(r1.xy)).z);
	r1.yz = asuint(-(int2)(NearFar_Menu_EffectThreshold.ww < asfloat(r1.zw)));
	r1.w = asuint((-asfloat(r1.w) + asfloat(0x3f800000u)));
	r0.z = (r0.z | r1.z);
	r3.xyz = (r0.zzz != 0u ? r12.yzw : r3.xyz);
	r4.xyz = (r0.zzz != 0u ? r12.yzw : r15.xyz);
	r5.xyz = asuint((-asfloat(r4.xyz) + asfloat(r12.yzw)));
	r1.z = (~r1.y);
	r3.w = asuint(-(int)(asfloat(0x38d1b717u) < asfloat(r1.x)));
	r1.x = asuint((asfloat(r1.x) * asfloat(0xbf599999u) + asfloat(0x3f733333u)));
	r1.z = (r1.z & r3.w);
	r3.w = (r1.z & 0x3b808081u);
	r2.w = asuint(saturate((asfloat(r2.w) + asfloat(r3.w))));
	r2.xyz = (r0.zzz != 0u ? uint3(0x00000000u, 0x00000000u, 0x00000000u) : r2.xyz);
	r7.z = (r1.z & r2.w);
	r7.w = asuint(-(int)(asfloat(r7.z) >= asfloat(0x3d008081u)));
	r13.w = 0x00000000u;
	r6.xyzw = (r0.zzzz != 0u ? r13.xwww : r7.xyzw);
	r0.z = asuint((InverseSize_MaxUV.x * asfloat(0x43000000u)));
	r7.z = asuint(saturate((asfloat(r0.x) / asfloat(r0.z))));
	r0.x = asuint((-asfloat(r6.y) + asfloat(r7.z)));
	r0.xz = asuint((-abs(asfloat(r0.xx)) * float2(asfloat(0x41a00000u), asfloat(0x42c80000u)) + float2(asfloat(0x3f800000u), asfloat(0x3f800000u))));
	r0.xz = asuint(max(asfloat(r0.xz), float2(asfloat(0x00000000u), asfloat(0x00000000u))));
	r5.xyz = asuint((asfloat(r0.xxx) * asfloat(r5.xyz) + asfloat(r4.xyz)));
	r3.xyz = asuint((asfloat(r3.xyz) + -asfloat(r5.xyz)));
	r1.z = asuint((-LowFrequency_HighFrequency_Sharpen_Overlay.y + LowFrequency_HighFrequency_Sharpen_Overlay.x));
	r1.z = asuint((asfloat(r7.z) * asfloat(r1.z) + LowFrequency_HighFrequency_Sharpen_Overlay.y));
	r0.x = asuint(min(asfloat(r0.x), asfloat(r1.z)));
	r7.y = asuint((asfloat(r0.z) * asfloat(r0.y)));
	r0.y = asuint((-asfloat(r0.x) + asfloat(0x3f7d70a4u)));
	r0.x = asuint((asfloat(r7.y) * asfloat(r0.y) + asfloat(r0.x)));
	o1.yz = r7.yz;
	r3.xyz = asuint(saturate((asfloat(r0.xxx) * asfloat(r3.xyz) + asfloat(r5.xyz))));
	r7.xyz = asuint((-asfloat(r4.xyz) + asfloat(r3.xyz)));
	r3.xyz = asuint(saturate((asfloat(r7.xyz) * LowFrequency_HighFrequency_Sharpen_Overlay.zzz + asfloat(r3.xyz))));
	r4.xyz = asuint((-asfloat(r3.xyz) + asfloat(r4.xyz)));
	r3.xyz = asuint(saturate((LowFrequency_HighFrequency_Sharpen_Overlay.www * asfloat(r4.xyz) + asfloat(r3.xyz))));
	r0.y = asuint((NearFar_Menu_EffectThreshold.x + NearFar_Menu_EffectThreshold.y));
	r0.z = asuint((-NearFar_Menu_EffectThreshold.x + NearFar_Menu_EffectThreshold.y));
	r0.y = asuint((-asfloat(r0.w) * asfloat(r0.z) + asfloat(r0.y)));
	r0.w = asuint(dot(NearFar_Menu_EffectThreshold.xx, NearFar_Menu_EffectThreshold.yy));
	r0.y = asuint((asfloat(r0.w) / asfloat(r0.y)));
	r0.y = asuint((asfloat(r0.y) + -NearFar_Menu_EffectThreshold.x));
	r0.y = asuint((asfloat(r0.y) / asfloat(r0.z)));
	r0.y = asuint((asfloat(r0.y) * asfloat(0x437a0000u)));
	r0.y = asuint(max(asfloat(r0.y), asfloat(0x3dcccccdu)));
	r0.y = asuint(min(asfloat(r0.y), asfloat(0x3f733333u)));
	r0.y = asuint((asfloat(r0.y) * asfloat(r1.x)));
	r4.xyz = asuint((asfloat(r2.xyz) + -asfloat(r5.xyz)));
	r0.yzw = asuint((asfloat(r0.yyy) * asfloat(r4.xyz) + asfloat(r5.xyz)));
	r4.xyz = asuint((asfloat(r4.xyz) * float3(asfloat(0x3f733333u), asfloat(0x3f733333u), asfloat(0x3f733333u)) + asfloat(r5.xyz)));
	r1.x = asuint(-(int)(NearFar_Menu_EffectThreshold.z == asfloat(0x00000000u)));
	r0.yzw = asuint(saturate(asfloat((r1.xxx != 0u ? r0.yzw : r4.xyz))));
	r3.xyz = (r6.www != 0u ? r0.yzw : r3.xyz);
	r0.y = asuint(dot(asfloat(r0.zwy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	o0.xyz = r3.xyz;
	o2.xyz = (r1.yyy != 0u ? r2.xyz : r3.xyz);
	o0.w = 0x3f800000u;
	r0.z = asuint((-asfloat(r13.x) + asfloat(r6.x)));
	r0.w = asuint((asfloat(r0.x) * asfloat(r0.z) + asfloat(r13.x)));
	r0.x = asuint((asfloat(r0.z) * asfloat(r0.x)));
	r0.x = asuint(-(int)(abs(asfloat(r0.x)) < asfloat(0x3c23d70au)));
	r0.x = (r0.x != 0u ? r13.x : r0.w);
	r0.x = (r6.w != 0u ? r0.y : r0.x);
	o2.w = asuint(min(asfloat(r6.z), asfloat(0x3d008081u)));
	o1.x = asuint(saturate((asfloat(r1.w) * asfloat(r0.x))));
	o1.w = 0x3f800000u;
#	else
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0, r16 = 0, r17 = 0, r18 = 0, r19 = 0;
	r0.xy = asuint((v1.xy + -SampleWeights4567.xy));
	r0.zw = asuint((v1.xy + SampleWeights4567.xy));
	r1.xy = asuint((asfloat(r0.zw) * TAA_DR1.xy));
	r1.xy = asuint(max(asfloat(r1.xy), float2(asfloat(0x00000000u), asfloat(0x00000000u))));
	r2.x = asuint(TAA_DR2.z);
	r2.y = asuint(TAA_DR1.y);
	r1.xy = asuint(min(asfloat(r1.xy), asfloat(r2.xy)));
	r1.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r1.xy)).x);
	r3.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r1.xy)).yxz);
	r4.xyzw = asuint((SampleWeights4567.xyxy * float4(asfloat(0x3f800000u), asfloat(0xbf800000u), asfloat(0x3f800000u), asfloat(0x00000000u)) + v1.xyxy));
	r5.xyzw = asuint((asfloat(r4.xyzw) * TAA_DR1.xyxy));
	r5.xyzw = asuint(max(asfloat(r5.xyzw), float4(asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u))));
	r5.xyzw = asuint(min(asfloat(r2.xyxy), asfloat(r5.xyzw)));
	r1.x = asuint(TAA_T3.Sample(TAA_S3, asfloat(r5.xy)).x);
	r1.y = asuint(min(asfloat(r1.z), asfloat(r1.x)));
	r1.zw = asuint((asfloat(r0.xy) * TAA_DR1.xy));
	r1.zw = asuint(max(asfloat(r1.zw), float2(asfloat(0x00000000u), asfloat(0x00000000u))));
	r1.zw = asuint(min(asfloat(r2.xy), asfloat(r1.zw)));
	r2.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r1.zw)).x);
	r1.y = asuint(min(asfloat(r1.y), asfloat(r2.z)));
	r2.z = asuint(-(int)(asfloat(r1.y) == asfloat(r2.z)));
	r0.xy = (r2.zz != 0u ? r0.xy : r0.zw);
	r0.z = asuint(-(int)(asfloat(r1.y) == asfloat(r1.x)));
	r0.xy = (r0.zz != 0u ? r4.xy : r0.xy);
	r6.xyzw = asuint((SampleWeights4567.xyxy * float4(asfloat(0x00000000u), asfloat(0xbf800000u), asfloat(0xbf800000u), asfloat(0x3f800000u)) + v1.xyxy));
	r7.xyzw = asuint((asfloat(r6.xyzw) * TAA_DR1.xyxy));
	r7.xyzw = asuint(max(asfloat(r7.xyzw), float4(asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u))));
	r7.xyzw = asuint(min(asfloat(r2.xyxy), asfloat(r7.xyzw)));
	r0.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r7.xy)).x);
	r0.w = asuint(min(asfloat(r1.y), asfloat(r0.z)));
	r1.x = asuint(TAA_T3.Sample(TAA_S3, asfloat(r5.zw)).x);
	r0.w = asuint(min(asfloat(r0.w), asfloat(r1.x)));
	r1.x = asuint(-(int)(asfloat(r0.w) == asfloat(r1.x)));
	r0.xy = (r1.xx != 0u ? r4.zw : r0.xy);
	r0.z = asuint(-(int)(asfloat(r0.w) == asfloat(r0.z)));
	r0.xy = (r0.zz != 0u ? r6.xy : r0.xy);
	r4.xyzw = asuint((SampleWeights4567.xyxy * float4(asfloat(0xbf800000u), asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x3f800000u)) + v1.xyxy));
	r8.xyzw = asuint((asfloat(r4.xyzw) * TAA_DR1.xyxy));
	r8.xyzw = asuint(max(asfloat(r8.xyzw), float4(asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u), asfloat(0x00000000u))));
	r8.xyzw = asuint(min(asfloat(r2.xyxy), asfloat(r8.xyzw)));
	r0.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r8.xy)).x);
	r0.w = asuint(min(asfloat(r0.w), asfloat(r0.z)));
	r1.x = asuint(TAA_T3.Sample(TAA_S3, asfloat(r7.zw)).x);
	r0.w = asuint(min(asfloat(r0.w), asfloat(r1.x)));
	r1.x = asuint(-(int)(asfloat(r0.w) == asfloat(r1.x)));
	r0.xy = (r1.xx != 0u ? r6.zw : r0.xy);
	r0.z = asuint(-(int)(asfloat(r0.w) == asfloat(r0.z)));
	r0.xy = (r0.zz != 0u ? r4.xy : r0.xy);
	r1.xy = asuint((v1.xy * TAA_DR1.xy));
	r1.xy = asuint(max(asfloat(r1.xy), float2(asfloat(0x00000000u), asfloat(0x00000000u))));
	r1.xy = asuint(min(asfloat(r2.xy), asfloat(r1.xy)));
	r0.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r1.xy)).x);
	r0.w = asuint(min(asfloat(r0.w), asfloat(r0.z)));
	r2.z = asuint(TAA_T3.Sample(TAA_S3, asfloat(r8.zw)).x);
	r0.w = asuint(min(asfloat(r0.w), asfloat(r2.z)));
	r2.z = asuint(-(int)(asfloat(r0.w) == asfloat(r2.z)));
	r0.z = asuint(-(int)(asfloat(r0.w) == asfloat(r0.z)));
	r0.xy = (r2.zz != 0u ? r4.zw : r0.xy);
	r0.xy = (r0.zz != 0u ? asuint(v1.xy) : r0.xy);
	r0.xy = asuint((asfloat(r0.xy) * TAA_DR1.xy));
	r0.xy = asuint(max(asfloat(r0.xy), float2(asfloat(0x00000000u), asfloat(0x00000000u))));
	r0.xy = asuint(min(asfloat(r2.xy), asfloat(r0.xy)));
	r0.xy = asuint(TAA_T2.Sample(TAA_S2, asfloat(r0.xy)).xy);
	r0.zw = asuint((asfloat(r0.xy) + v1.xy));
	r0.x = asuint(dot(asfloat(r0.xy), asfloat(r0.xy)));
	r0.x = asuint(sqrt(asfloat(r0.x)));
	r2.xy = asuint((asfloat(r0.zw) * TAA_DR1.zw));
	r2.xy = asuint(max(asfloat(r2.xy), float2(asfloat(0x00000000u), asfloat(0x00000000u))));
	r4.x = asuint(min(asfloat(r2.x), TAA_DR2.w));
	r4.y = asuint(min(asfloat(r2.y), TAA_DR1.w));
	r2.xyw = asuint(TAA_T1.Sample(TAA_S1, asfloat(r4.xy)).xyz);
	r3.w = asuint(dot(asfloat(r3.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r0.y = asuint(-(int)(asfloat(r3.w) < asfloat(r2.x)));
	r4.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r1.zw)).yxz);
	r1.z = asuint(TAA_T4.Sample(TAA_S4, asfloat(r1.zw)).x);
	r4.w = asuint(dot(asfloat(r4.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r1.w = asuint(-(int)(asfloat(r4.w) < asfloat(r2.x)));
	r6.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r5.xy)).yxz);
	r6.w = asuint(dot(asfloat(r6.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r3.x = asuint(-(int)(asfloat(r6.w) < asfloat(r2.x)));
	r9.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r5.zw)).yxz);
	r9.w = asuint(dot(asfloat(r9.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r4.x = asuint(-(int)(asfloat(r9.w) < asfloat(r2.x)));
	r10.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r7.xy)).yxz);
	r10.w = asuint(dot(asfloat(r10.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r6.x = asuint(-(int)(asfloat(r10.w) < asfloat(r2.x)));
	r11.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r7.zw)).yxz);
	r11.w = asuint(dot(asfloat(r11.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r9.x = asuint(-(int)(asfloat(r11.w) < asfloat(r2.x)));
	r12.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r8.xy)).yxz);
	r12.w = asuint(dot(asfloat(r12.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r10.x = asuint(-(int)(asfloat(r12.w) < asfloat(r2.x)));
	r13.xyz = asuint(TAA_T0.Sample(TAA_S0, asfloat(r8.zw)).yxz);
	r13.w = asuint(dot(asfloat(r13.xzy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r14.x = asuint(-(int)(asfloat(r13.w) < asfloat(r2.x)));
	r14.yzw = asuint(TAA_T0.Sample(TAA_S0, asfloat(r1.xy)).xyz);
	r1.x = asuint(TAA_T4.Sample(TAA_S4, asfloat(r1.xy)).x);
	r15.x = asuint(dot(asfloat(r14.zwy), float3(asfloat(0x3f000000u), asfloat(0x3e800000u), asfloat(0x3e800000u))));
	r1.y = asuint(-(int)(asfloat(r15.x) < asfloat(r2.x)));
	r15.yz = r14.yw;
	r16.x = asuint(-(int)(asfloat(r15.x) < asfloat(0x3f8020c5u)));
	r16.xyz = (r16.xxx != 0u ? r15.yzx : uint3(0x3f8020c5u, 0x3f8020c5u, 0x3f8020c5u));
	r16.xyz = (r1.yyy != 0u ? uint3(0x3f8020c5u, 0x3f8020c5u, 0x3f8020c5u) : r16.xyz);
	r16.w = asuint(-(int)(asfloat(r13.w) < asfloat(r16.z)));
	r17.xyz = (r16.www != 0u ? r13.yzw : r16.xyz);
	r16.xyz = (r14.xxx != 0u ? r16.xyz : r17.xyz);
	r17.xyz = asuint((asfloat(r12.yxz) * SampleWeights0123.zzz));
	r17.xyz = asuint((asfloat(r11.yxz) * SampleWeights0123.www + asfloat(r17.xyz)));
	r17.xyz = asuint((asfloat(r13.yxz) * SampleWeights0123.yyy + asfloat(r17.xyz)));
	r17.xyz = asuint((asfloat(r14.yzw) * SampleWeights0123.xxx + asfloat(r17.xyz)));
	r11.x = asuint(-(int)(asfloat(r12.w) < asfloat(r16.z)));
	r18.xyz = (r11.xxx != 0u ? r12.yzw : r16.xyz);
	r16.xyz = (r10.xxx != 0u ? r16.xyz : r18.xyz);
	r11.x = asuint(-(int)(asfloat(r11.w) < asfloat(r16.z)));
	r18.xyz = (r11.xxx != 0u ? r11.yzw : r16.xyz);
	r16.xyz = (r9.xxx != 0u ? r16.xyz : r18.xyz);
	r11.x = asuint(-(int)(asfloat(r10.w) < asfloat(r16.z)));
	r18.xyz = (r11.xxx != 0u ? r10.yzw : r16.xyz);
	r16.xyz = (r6.xxx != 0u ? r16.xyz : r18.xyz);
	r11.x = asuint(-(int)(asfloat(r9.w) < asfloat(r16.z)));
	r18.xyz = (r11.xxx != 0u ? r9.yzw : r16.xyz);
	r16.xyz = (r4.xxx != 0u ? r16.xyz : r18.xyz);
	r11.x = asuint(-(int)(asfloat(r6.w) < asfloat(r16.z)));
	r18.xyz = (r11.xxx != 0u ? r6.yzw : r16.xyz);
	r16.xyz = (r3.xxx != 0u ? r16.xyz : r18.xyz);
	r11.x = asuint(-(int)(asfloat(r4.w) < asfloat(r16.z)));
	r18.xyz = (r11.xxx != 0u ? r4.yzw : r16.xyz);
	r16.yzw = (r1.www != 0u ? r16.xyz : r18.xyz);
	r11.x = asuint(-(int)(asfloat(r3.w) < asfloat(r16.w)));
	r18.yzw = (r11.xxx != 0u ? r3.yzw : r16.yzw);
	r11.x = asuint(-(int)(asfloat(0xba83126fu) < asfloat(r15.x)));
	r19.xyz = (r11.xxx != 0u ? r15.yzx : uint3(0xba83126fu, 0xba83126fu, 0xba83126fu));
	r19.xyz = (r1.yyy != 0u ? r19.xyz : uint3(0xba83126fu, 0xba83126fu, 0xba83126fu));
	r1.y = asuint(-(int)(asfloat(r19.z) < asfloat(r13.w)));
	r13.xyz = (r1.yyy != 0u ? r13.yzw : r19.xyz);
	r13.xyz = (r14.xxx != 0u ? r13.xyz : r19.xyz);
	r1.y = asuint((-asfloat(r13.w) + asfloat(r15.x)));
	r1.y = asuint((-abs(asfloat(r1.y)) + asfloat(0x3e4ccccdu)));
	r11.x = asuint(-(int)(asfloat(r13.z) < asfloat(r12.w)));
	r12.xyz = (r11.xxx != 0u ? r12.yzw : r13.xyz);
	r12.xyz = (r10.xxx != 0u ? r12.xyz : r13.xyz);
	r10.x = asuint((-asfloat(r12.w) + asfloat(r15.x)));
	r10.x = asuint((-abs(asfloat(r10.x)) + asfloat(0x3e4ccccdu)));
	r10.x = asuint(ceil(asfloat(r10.x)));
	r11.x = asuint(-(int)(asfloat(r12.z) < asfloat(r11.w)));
	r11.xyz = (r11.xxx != 0u ? r11.yzw : r12.xyz);
	r11.xyz = (r9.xxx != 0u ? r11.xyz : r12.xyz);
	r9.x = asuint((-asfloat(r11.w) + asfloat(r15.x)));
	r9.x = asuint((-abs(asfloat(r9.x)) + asfloat(0x3e4ccccdu)));
	r9.x = asuint(ceil(asfloat(r9.x)));
	r11.w = asuint(-(int)(asfloat(r11.z) < asfloat(r10.w)));
	r12.xyz = (r11.www != 0u ? r10.yzw : r11.xyz);
	r11.xyz = (r6.xxx != 0u ? r12.xyz : r11.xyz);
	r6.x = asuint((-asfloat(r10.w) + asfloat(r15.x)));
	r6.x = asuint((-abs(asfloat(r6.x)) + asfloat(0x3e4ccccdu)));
	r6.x = asuint(ceil(asfloat(r6.x)));
	r10.y = asuint(-(int)(asfloat(r11.z) < asfloat(r9.w)));
	r10.yzw = (r10.yyy != 0u ? r9.yzw : r11.xyz);
	r10.yzw = (r4.xxx != 0u ? r10.yzw : r11.xyz);
	r4.x = asuint((-asfloat(r9.w) + asfloat(r15.x)));
	r4.x = asuint((-abs(asfloat(r4.x)) + asfloat(0x3e4ccccdu)));
	r4.x = asuint(ceil(asfloat(r4.x)));
	r9.y = asuint(-(int)(asfloat(r10.w) < asfloat(r6.w)));
	r9.yzw = (r9.yyy != 0u ? r6.yzw : r10.yzw);
	r9.yzw = (r3.xxx != 0u ? r9.yzw : r10.yzw);
	r3.x = asuint((-asfloat(r6.w) + asfloat(r15.x)));
	r3.x = asuint((-abs(asfloat(r3.x)) + asfloat(0x3e4ccccdu)));
	r3.x = asuint(ceil(asfloat(r3.x)));
	r6.y = asuint(-(int)(asfloat(r9.w) < asfloat(r4.w)));
	r6.yzw = (r6.yyy != 0u ? r4.yzw : r9.yzw);
	r6.yzw = (r1.www != 0u ? r6.yzw : r9.yzw);
	r1.w = asuint((-asfloat(r4.w) + asfloat(r15.x)));
	r1.w = asuint((-abs(asfloat(r1.w)) + asfloat(0x3e4ccccdu)));
	r1.yw = asuint(ceil(asfloat(r1.yw)));
	r18.x = r6.z;
	r4.y = asuint(-(int)(asfloat(r6.w) < asfloat(r3.w)));
	r4.yzw = (r4.yyy != 0u ? r3.yzw : r6.yzw);
	r11.xw = (r0.yy != 0u ? r4.yw : r6.yw);
	r16.x = r4.z;
	r12.xyzw = (r0.yyyy != 0u ? r16.xyzw : r18.xyzw);
	r0.y = asuint((-asfloat(r3.w) + asfloat(r15.x)));
	r0.y = asuint((-abs(asfloat(r0.y)) + asfloat(0x3e4ccccdu)));
	r0.y = asuint(ceil(asfloat(r0.y)));
	r0.y = asuint((-asfloat(r0.y) + asfloat(0x40800000u)));
	r0.y = asuint((-asfloat(r1.w) + asfloat(r0.y)));
	r0.y = asuint((-asfloat(r3.x) + asfloat(r0.y)));
	r0.y = asuint((-asfloat(r4.x) + asfloat(r0.y)));
	r0.y = asuint((-asfloat(r6.x) + asfloat(r0.y)));
	r0.y = asuint((-asfloat(r9.x) + asfloat(r0.y)));
	r0.y = asuint((-asfloat(r10.x) + asfloat(r0.y)));
	r0.y = asuint(saturate((-asfloat(r1.y) + asfloat(r0.y))));
	r1.y = asuint(-(int)(asfloat(0x3f800000u) < asfloat(r12.w)));
	r1.w = asuint((-asfloat(r12.y) * asfloat(0x3e800000u) + asfloat(r12.w)));
	r1.w = asuint((-asfloat(r12.z) * asfloat(0x3e800000u) + asfloat(r1.w)));
	r3.y = asuint((asfloat(r1.w) + asfloat(r1.w)));
	r11.z = r12.x;
	r3.xzw = r12.yzw;
	r1.w = asuint((-asfloat(r11.x) * asfloat(0x3e800000u) + asfloat(r11.w)));
	r1.w = asuint((-asfloat(r12.x) * asfloat(0x3e800000u) + asfloat(r1.w)));
	r11.y = asuint((asfloat(r1.w) + asfloat(r1.w)));
	r1.w = asuint(-(int)(asfloat(r11.w) < asfloat(0x00000000u)));
	r4.xyzw = (r1.wwww != 0u ? r3.xyzw : r11.xyzw);
	r6.xyzw = (r1.yyyy != 0u ? r4.xyzw : r3.xyzw);
	r1.y = asuint(max(asfloat(r2.x), asfloat(r4.w)));
	r9.x = asuint(min(asfloat(r6.w), asfloat(r1.y)));
	r9.z = r6.w;
	r9.y = r4.w;
	r10.z = r3.w;
	r10.x = r2.x;
	r1.y = asuint(TAA_T4.Sample(TAA_S4, asfloat(r5.xy)).x);
	r1.w = asuint(TAA_T4.Sample(TAA_S4, asfloat(r5.zw)).x);
	r1.y = asuint((asfloat(r1.y) + asfloat(r1.z)));
	r1.y = asuint(-(int)(asfloat(r1.y) < asfloat(0x40000000u)));
	r1.z = asuint(TAA_T4.Sample(TAA_S4, asfloat(r7.xy)).x);
	r3.w = asuint(TAA_T4.Sample(TAA_S4, asfloat(r7.zw)).x);
	r1.z = asuint((asfloat(r1.z) + asfloat(r1.w)));
	r1.z = asuint(-(int)(asfloat(r1.z) < asfloat(0x40000000u)));
	r1.y = (r1.z | r1.y);
	r1.z = asuint(TAA_T4.Sample(TAA_S4, asfloat(r8.xy)).x);
	r1.w = asuint(TAA_T4.Sample(TAA_S4, asfloat(r8.zw)).x);
	r1.w = asuint((asfloat(r1.x) + asfloat(r1.w)));
	r1.z = asuint((asfloat(r1.z) + asfloat(r3.w)));
	r1.zw = asuint(-(int2)(asfloat(r1.zw) < float2(asfloat(0x40000000u), asfloat(0x40000000u))));
	r1.y = (r1.z | r1.y);
	r1.y = (r1.w | r1.y);
	r1.x = (r1.y != 0u ? 0x00000000u : r1.x);
	r1.y = asuint((-asfloat(r1.x) + asfloat(0x3f800000u)));
	r1.x = asuint(-(int)(asfloat(0x00000000u) < asfloat(r1.x)));
	r1.z = asuint((asfloat(r2.y) * asfloat(0x3f733333u)));
	r0.y = asuint(saturate((asfloat(r0.y) * asfloat(0x3e800000u) + asfloat(r1.z))));
	r0.y = asuint((asfloat(r1.y) * asfloat(r0.y)));
	r1.y = asuint(-(int)(asfloat(r0.y) < asfloat(0x3f670a3du)));
	r10.y = r11.w;
	r4.xyz = (r1.yyy != 0u ? r4.xyz : r11.xyz);
	r2.xyz = (r1.yyy != 0u ? r9.xyz : r10.xyz);
	r1.yzw = (r1.yyy != 0u ? r6.xyz : r3.xyz);
	r1.yzw = asuint((-asfloat(r4.xyz) + asfloat(r1.yzw)));
	r2.yz = asuint((-asfloat(r2.yy) + asfloat(r2.zx)));
	r3.x = asuint(-(int)(asfloat(0x3c23d70au) < asfloat(r2.y)));
	r2.y = asuint((asfloat(r2.z) / asfloat(r2.y)));
	r2.y = (r3.x != 0u ? r2.y : 0x3f000000u);
	r1.yzw = asuint((asfloat(r2.yyy) * asfloat(r1.yzw) + asfloat(r4.xyz)));
	r2.y = asuint(min(asfloat(r0.w), asfloat(r0.z)));
	r0.zw = asuint(-(int2)(asfloat(r0.zw) >= float2(asfloat(0x3f800000u), asfloat(0x3f800000u))));
	r2.y = asuint(-(int)(asfloat(0x00000000u) >= asfloat(r2.y)));
	r0.z = (r0.z | r2.y);
	r0.z = (r0.w | r0.z);
	r0.z = (r1.x | r0.z);
	r1.xyz = (r0.zzz != 0u ? r14.yzw : r1.yzw);
	r3.xyz = (r0.zzz != 0u ? r14.yzw : r17.xyz);
	r4.xyz = asuint((-asfloat(r3.xyz) + asfloat(r14.yzw)));
	r15.w = 0x00000000u;
	r0.zw = (r0.zz != 0u ? r15.xw : r2.xw);
	r1.w = asuint((InverseSize_MaxUV.x * asfloat(0x43000000u)));
	r2.z = asuint(saturate((asfloat(r0.x) / asfloat(r1.w))));
	r0.x = asuint((-asfloat(r0.w) + asfloat(r2.z)));
	r0.z = asuint((-asfloat(r15.x) + asfloat(r0.z)));
	r0.xw = asuint((-abs(asfloat(r0.xx)) * float2(asfloat(0x41a00000u), asfloat(0x42c80000u)) + float2(asfloat(0x3f800000u), asfloat(0x3f800000u))));
	r0.xw = asuint(max(asfloat(r0.xw), float2(asfloat(0x00000000u), asfloat(0x00000000u))));
	r4.xyz = asuint((asfloat(r0.xxx) * asfloat(r4.xyz) + asfloat(r3.xyz)));
	r1.xyz = asuint((asfloat(r1.xyz) + -asfloat(r4.xyz)));
	r1.w = asuint((-LowFrequency_HighFrequency_Sharpen_Overlay.y + LowFrequency_HighFrequency_Sharpen_Overlay.x));
	r1.w = asuint((asfloat(r2.z) * asfloat(r1.w) + LowFrequency_HighFrequency_Sharpen_Overlay.y));
	r0.x = asuint(min(asfloat(r0.x), asfloat(r1.w)));
	r2.y = asuint(saturate((asfloat(r0.w) * asfloat(r0.y))));
	r0.y = asuint((-asfloat(r0.x) + asfloat(0x3f7d70a4u)));
	r0.x = asuint((asfloat(r2.y) * asfloat(r0.y) + asfloat(r0.x)));
	o1.yz = r2.yz;
	r1.xyz = asuint(saturate((asfloat(r0.xxx) * asfloat(r1.xyz) + asfloat(r4.xyz))));
	r2.xyz = asuint((-asfloat(r3.xyz) + asfloat(r1.xyz)));
	r1.xyz = asuint(saturate((asfloat(r2.xyz) * LowFrequency_HighFrequency_Sharpen_Overlay.zzz + asfloat(r1.xyz))));
	r2.xyz = asuint((-asfloat(r1.xyz) + asfloat(r3.xyz)));
	o0.xyz = asuint(saturate((LowFrequency_HighFrequency_Sharpen_Overlay.www * asfloat(r2.xyz) + asfloat(r1.xyz))));
	o0.w = 0x3f800000u;
	r0.y = asuint((asfloat(r0.x) * asfloat(r0.z) + asfloat(r15.x)));
	r0.x = asuint((asfloat(r0.z) * asfloat(r0.x)));
	r0.x = asuint(-(int)(abs(asfloat(r0.x)) < asfloat(0x3c23d70au)));
	o1.x = asuint(saturate(asfloat((r0.x != 0u ? r15.x : r0.y))));
	o1.w = 0x3f800000u;
#	endif
	TAA_FORM_OUTPUT psout;
	psout.Color = asfloat(o0);
	psout.History = asfloat(o1);
#	if defined(TAA_BLEND_WATER)
	psout.WaterHistory = asfloat(o2);
#	endif
	return psout;
}
#endif

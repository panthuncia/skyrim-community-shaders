#if defined(CS_ORG_BINDLESS)
// The ORG build (Drawcall Limit Fix's blood splatter family, Features/DrawcallLimitFix/Features/BloodSplatter). One push block
// serves both stages: a draw's latch region, which holds the constants the engine's SetupGeometry wrote (VS b2 c0-c5, PS b2 c0),
// the textures' and samplers' heap indices and the geometry.
#	include "Common/OrgResources.hlsli"

struct BloodSplatterOrgDraw
{
	float4 WorldViewProjRows[4];  // VS b2 c0-c3, row_major there
	float4 LightLoc;              // VS b2 c4
	float4 Ctrl;                  // VS b2 c5: x
	float4 Alpha;                 // PS b2 c0: x
	uint4 Textures;               // t0-t3 heap indices
	uint4 Samplers;               // s0-s3 sampler heap indices
	uint2 VertexAddress;
	uint2 IndexAddress;
	uint2 VertexLayout;
	uint VertexStride;
	uint FirstIndex;
};

#	define BLOOD_SPLATTER_ORG_RESOURCES(X) \
		X(LATCH, BloodSplatterOrgDraw, BloodDraw, 2)
ORG_PUSH_BLOCK(BLOOD_SPLATTER_ORG_RESOURCES(ORG_HANDLE))
BLOOD_SPLATTER_ORG_RESOURCES(ORG_RESOURCE)
#endif

struct VS_INPUT
{
	float4 Position: POSITION0;
	float2 TexCoord: TEXCOORD0;
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
	float3 TexCoordColor: TEXCOORD0;

#if defined(SPLATTER)
	float2 TexCoordAlpha: TEXCOORD1;
#endif
};

#ifdef VSHADER
#	if defined(CS_ORG_BINDLESS)
static float4x4 WorldViewProj = float4x4(BloodDraw.WorldViewProjRows[0], BloodDraw.WorldViewProjRows[1], BloodDraw.WorldViewProjRows[2], BloodDraw.WorldViewProjRows[3]);
static float4 LightLoc = BloodDraw.LightLoc;
static float Ctrl = BloodDraw.Ctrl.x;  // fBloodSplatterFlareOffsetScale
#	else
cbuffer PerGeometry : register(b2)
{
	row_major float4x4 WorldViewProj : packoffset(c0);
	float4 LightLoc : packoffset(c4);
	float Ctrl : packoffset(c5);  // fBloodSplatterFlareOffsetScale
};
#	endif

#	if defined(CS_ORG_BINDLESS)
VS_OUTPUT BloodSplatterVertex(VS_INPUT input)
#	else
VS_OUTPUT main(VS_INPUT input)
#	endif
{
	VS_OUTPUT vsout;

	float4 projectedPosition = mul(WorldViewProj, float4(input.Position.xy, 0.0, 1.0));
	float2 lightOffset = (-LightLoc.xy + projectedPosition.xy) * LightLoc.ww;

#	if defined(SPLATTER)
	vsout.TexCoordAlpha = -(lightOffset.xy * float2(-0.5, 0.5)) + input.TexCoord;
#	elif defined(FLARE)
	projectedPosition.xy += lightOffset * Ctrl.xx;
#	endif

	vsout.Position = projectedPosition;
	vsout.TexCoordColor = float3(input.TexCoord, input.Position.z);

	return vsout;
}

#	if defined(CS_ORG_BINDLESS)
#		if defined(__spirv__)
#			define DCLF_PULLED_VERTICES
#			include "Common/DCLFVertexPull.hlsli"

/** @brief The draw's vertex a_vertexID, pulled as the input assembler reads it: its 16-bit index, then its attributes. */
VS_INPUT BloodSplatterPullVertex(uint a_vertexID)
{
	DCLFDrawVertexLayout = BloodDraw.VertexLayout;
	const uint indexNumber = BloodDraw.FirstIndex + a_vertexID;
	const uint indexWord = vk::RawBufferLoad<uint>(DCLFAddress(BloodDraw.IndexAddress) + uint64_t((indexNumber * 2) & ~3u));
	const uint index = (indexNumber & 1) ? (indexWord >> 16) : (indexWord & 0xFFFF);
	DCLFStream first = { DCLFAddress(BloodDraw.VertexAddress), BloodDraw.VertexStride };
	DCLFStream second = { uint64_t(0), 0u };
	DCLFStreamsAt(index, first, second);

	VS_INPUT input;
	input.Position = DCLFPullPosition(first, second);
	input.TexCoord = DCLFPullHalf2(1, first, second);
	return input;
}
#		else
// The D3D12 build has no vertex pulling yet: it compiles, and draws nothing.
VS_INPUT BloodSplatterPullVertex(uint a_vertexID)
{
	VS_INPUT input = (VS_INPUT)0;
	return input;
}
#		endif

VS_OUTPUT main(uint a_vertexID: SV_VertexID)
{
	return BloodSplatterVertex(BloodSplatterPullVertex(a_vertexID));
}
#	endif
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#ifdef PSHADER
#	if defined(CS_ORG_BINDLESS)
static SamplerState SampBloodColor = SamplerDescriptorHeap[BloodDraw.Samplers.x];
static SamplerState SampBloodAlpha = SamplerDescriptorHeap[BloodDraw.Samplers.y];
static SamplerState SampFlareColor = SamplerDescriptorHeap[BloodDraw.Samplers.z];
static SamplerState SampFlareHDR = SamplerDescriptorHeap[BloodDraw.Samplers.w];

static Texture2D<float4> TexBloodColor = ResourceDescriptorHeap[BloodDraw.Textures.x];
static Texture2D<float4> TexBloodAlpha = ResourceDescriptorHeap[BloodDraw.Textures.y];
static Texture2D<float4> TexFlareColor = ResourceDescriptorHeap[BloodDraw.Textures.z];
static Texture2D<float4> TexFlareHDR = ResourceDescriptorHeap[BloodDraw.Textures.w];

static float Alpha = BloodDraw.Alpha.x;
#	else
SamplerState SampBloodColor : register(s0);
SamplerState SampBloodAlpha : register(s1);
SamplerState SampFlareColor : register(s2);
SamplerState SampFlareHDR : register(s3);

Texture2D<float4> TexBloodColor : register(t0);
Texture2D<float4> TexBloodAlpha : register(t1);
Texture2D<float4> TexFlareColor : register(t2);
Texture2D<float4> TexFlareHDR : register(t3);

cbuffer PerGeometry : register(b2)
{
	float Alpha : packoffset(c0);
};
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#	if defined(SPLATTER)
	float3 bloodAplha = TexBloodAlpha.Sample(SampBloodAlpha, input.TexCoordAlpha.xy).xyz;
	float4 bloodColor = TexBloodColor.Sample(SampBloodColor, input.TexCoordColor.xy).xyzw;

	float colorMul = bloodColor.w * (Alpha * input.TexCoordColor.z);
	float3 adjustedBloodColor = colorMul * (bloodColor.xyz - 1.0.xxx) + 1.0.xxx;
	float3 adjustedBloodAlpha = colorMul * (-adjustedBloodColor.xyz + bloodAplha);

	psout.Color = float4(adjustedBloodAlpha + adjustedBloodColor, 1.0);
#	elif defined(FLARE)
	float flareMult = TexFlareColor.Sample(SampFlareColor, input.TexCoordColor.xy).x;
	float4 colorHDR = TexFlareHDR.Sample(SampFlareHDR, input.TexCoordColor.xy).xyzw;

	psout.Color = (colorHDR * flareMult) * Alpha;
#	endif

	return psout;
}
#endif

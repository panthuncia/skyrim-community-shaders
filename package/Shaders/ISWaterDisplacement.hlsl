#if defined(HEIGHTMAP_SMOOTH) || defined(HEIGHTMAP_RAIN) || defined(HEIGHTMAP_WADING)
#	define HEIGHTMAP
#endif

#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): the slots each form reads from the heap, the
// shared constants, and PerGeometry from the engine-constants capture through the pass's latch. The vertex stage is the graph's
// fullscreen triangle: the heightmaps' four neighbour coordinates are made here instead, and the ripple makers (which draw their own
// geometry) have no ORG build.
#	define ORG_SHARED_CONSTANTS
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	if defined(BLEND_HEIGHTMAPS)
#		define ISWATERDISPLACEMENT_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, HeightMap01Tex, 0) X(SRV, Texture2D<float4>, HeightMap02Tex, 1) X(SAMPLER, SamplerState, HeightMap01Sampler, 0) X(SAMPLER, SamplerState, HeightMap02Sampler, 1) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
#	elif defined(NORMALS)
#		define ISWATERDISPLACEMENT_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, DisplaceMapTex, 0) X(SAMPLER, SamplerState, DisplaceMapSampler, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
#	elif defined(TEXCOORD_OFFSET)
#		define ISWATERDISPLACEMENT_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, DisplaySamplerTex, 0) X(SAMPLER, SamplerState, DisplaySamplerSampler, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
#	elif defined(HEIGHTMAP)
#		define ISWATERDISPLACEMENT_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, HeightMapTex, 0) X(SAMPLER, SamplerState, HeightMapSampler, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
#	else
#		define ISWATERDISPLACEMENT_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
#	endif
ORG_PUSH_BLOCK(ISWATERDISPLACEMENT_ORG_RESOURCES(ORG_HANDLE))
ISWATERDISPLACEMENT_ORG_RESOURCES(ORG_RESOURCE)
#endif

struct VS_INPUT
{
	float4 Position: POSITION0;
	float2 TexCoord: TEXCOORD0;
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
	float2 TexCoord: TEXCOORD0;
#if defined(HEIGHTMAP) && !defined(CS_ORG_BINDLESS)
	float2 TexCoord1: TEXCOORD1;
	float2 TexCoord2: TEXCOORD2;
	float2 TexCoord3: TEXCOORD3;
	float2 TexCoord4: TEXCOORD4;
#endif
};

#ifdef VSHADER
#	if defined(RIPPLE_MAKER_WADING)
cbuffer PerGeometry : register(b2)
{
	// The two rows of a 2D affine transform that places the ripple quad.
	float4 TranslationMatrix0 : packoffset(c1);
	float4 TranslationMatrix1 : packoffset(c2);
};
#	endif

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	vsout.Position = float4(input.Position.xyz, 1);
#	if defined(RIPPLE_MAKER_WADING)
	vsout.Position.x = dot(float3(input.Position.xy, 1), TranslationMatrix0.xyz);
	vsout.Position.y = dot(float3(input.Position.xy, 1), TranslationMatrix1.xyz);
#	endif
	vsout.TexCoord = input.TexCoord.xy;
#	if defined(HEIGHTMAP)
	vsout.TexCoord1 = input.TexCoord.xy + float2(-0.0009765625, -0.0009765625);
	vsout.TexCoord2 = input.TexCoord.xy + float2(-0.0009765625, 0.0009765625);
	vsout.TexCoord3 = input.TexCoord.xy + float2(0.0009765625, -0.0009765625);
	vsout.TexCoord4 = input.TexCoord.xy + float2(0.0009765625, 0.0009765625);
#	endif

	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
static float4 PerGeometryRow0 = OrgEngineRow(PerGeometryAt, 0);
static float4 PerGeometryRow1 = OrgEngineRow(PerGeometryAt, 1);
static float Time = PerGeometryRow0.x;
static float BlendAmount = PerGeometryRow0.y;
static float2 TextureOffset = PerGeometryRow0.zw;
static float fDamp = PerGeometryRow1.x;
static float3 RainVars = PerGeometryRow1.yzw;
static float4 WadingVars = OrgEngineRow(PerGeometryAt, 2);
#	else
#		if defined(BLEND_HEIGHTMAPS)
SamplerState HeightMap01Sampler : register(s0);
SamplerState HeightMap02Sampler : register(s1);
#		elif defined(NORMALS)
SamplerState DisplaceMapSampler : register(s0);
#		elif defined(TEXCOORD_OFFSET)
SamplerState DisplaySamplerSampler : register(s0);
#		elif defined(HEIGHTMAP)
SamplerState HeightMapSampler : register(s0);
#		endif

#		if defined(BLEND_HEIGHTMAPS)
Texture2D<float4> HeightMap01Tex : register(t0);
Texture2D<float4> HeightMap02Tex : register(t1);
#		elif defined(NORMALS)
Texture2D<float4> DisplaceMapTex : register(t0);
#		elif defined(TEXCOORD_OFFSET)
Texture2D<float4> DisplaySamplerTex : register(t0);
#		elif defined(HEIGHTMAP)
Texture2D<float4> HeightMapTex : register(t0);
#		endif

cbuffer PerGeometry : register(b2)
{
	float Time : packoffset(c0.x);
	float BlendAmount : packoffset(c0.y);
	float2 TextureOffset : packoffset(c0.z);
	float fDamp : packoffset(c1.x);
	float3 RainVars : packoffset(c1.y);
	float4 WadingVars : packoffset(c2.x);
};
#	endif

#	if defined(NORMALS)
float GetDisplacementNormalValue(float2 texCoord)
{
	float displaceValue = DisplaceMapTex.Sample(DisplaceMapSampler, texCoord).x;
	return fDamp * pow(abs(displaceValue), 7);
}
#	endif

#	if defined(HEIGHTMAP)
#		if defined(CS_ORG_BINDLESS)
// The vertex shader's four neighbour coordinates, made here: the graph's fullscreen triangle writes TEXCOORD0 only.
#			define HEIGHTMAP_TEXCOORD1(input) ((input).TexCoord + float2(-0.0009765625, -0.0009765625))
#			define HEIGHTMAP_TEXCOORD2(input) ((input).TexCoord + float2(-0.0009765625, 0.0009765625))
#			define HEIGHTMAP_TEXCOORD3(input) ((input).TexCoord + float2(0.0009765625, -0.0009765625))
#			define HEIGHTMAP_TEXCOORD4(input) ((input).TexCoord + float2(0.0009765625, 0.0009765625))
#		else
#			define HEIGHTMAP_TEXCOORD1(input) (input).TexCoord1
#			define HEIGHTMAP_TEXCOORD2(input) (input).TexCoord2
#			define HEIGHTMAP_TEXCOORD3(input) (input).TexCoord3
#			define HEIGHTMAP_TEXCOORD4(input) (input).TexCoord4
#		endif
float4 GetHeight(PS_INPUT input, uniform float3 Vars)
{
	float2 height = HeightMapTex.Sample(HeightMapSampler, input.TexCoord).xy;
	float heightLB = HeightMapTex.Sample(HeightMapSampler, HEIGHTMAP_TEXCOORD1(input)).x;
	float heightLT = HeightMapTex.Sample(HeightMapSampler, HEIGHTMAP_TEXCOORD2(input)).x;
	float heightRB = HeightMapTex.Sample(HeightMapSampler, HEIGHTMAP_TEXCOORD3(input)).x;
	float heightRT = HeightMapTex.Sample(HeightMapSampler, HEIGHTMAP_TEXCOORD4(input)).x;

	float heightDispersion = -height.x * 4 + (heightLB + heightLT + heightRB + heightRT);
	float tmp1 = Vars.x * heightDispersion + (height.y - 0.5);
	float heightLR = heightLB + heightLT - heightRB - heightRT;
	float heightBT = heightLB - heightLT + heightRB - heightRT;
	return 0.5 + float4(max(-1, Vars.z * float2(Vars.y * tmp1 + (height.x - 0.5), tmp1)), 0.5 * float2(heightLR, heightBT));
}
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#	if defined(BLEND_HEIGHTMAPS)
	float height01 = HeightMap01Tex.Sample(HeightMap01Sampler, input.TexCoord).x;
	float height02 = HeightMap02Tex.Sample(HeightMap02Sampler, input.TexCoord).x;

	psout.Color.xyz = lerp((0.8 / fDamp) * abs(height01), abs(height02), BlendAmount);
	psout.Color.w = 1;
#	elif defined(CLEAR_SIMULATION)
	psout.Color = 0.5;
#	elif defined(NORMALS)

	float offset = 0.001953125;
	float valueRL = 0;
	float valueTB = 0;
	[unroll] for (int i = -1; i <= 1; ++i)
	{
		[unroll] for (int j = -1; j <= 1; ++j)
		{
			if (i == 0 && j == 0) {
				continue;
			}

			float currentValue = GetDisplacementNormalValue(input.TexCoord + float2(i * offset, j * offset));

			float centerMul = 1;
			if (i == 0 || j == 0) {
				centerMul = 2;
			}

			valueRL += i * centerMul * currentValue;
			valueTB += j * centerMul * currentValue;
		}
	}

	psout.Color = float4(normalize(float3(-valueRL, valueTB, 1)), 1) * 0.5 + 0.5;

#	elif defined(RIPPLE_MAKER_RAIN) || defined(RIPPLE_MAKER_WADING)
	psout.Color = float4(1, 0.5, 0.5, 0.5);
#	elif defined(TEXCOORD_OFFSET)
	float lerpFactor = saturate(10 * (-0.4 + length(input.TexCoord - 0.5)));
	float4 displayColor = DisplaySamplerTex.Sample(DisplaySamplerSampler, TextureOffset + input.TexCoord);
	psout.Color.xy = displayColor.xy;
	psout.Color.zw = lerp(displayColor.zw, 0.5, lerpFactor);
#	elif defined(HEIGHTMAP_SMOOTH)
	psout.Color = HeightMapTex.Sample(HeightMapSampler, input.TexCoord);
#	elif defined(HEIGHTMAP_RAIN)
	psout.Color = GetHeight(input, RainVars);
#	elif defined(HEIGHTMAP_WADING)
	psout.Color = GetHeight(input, WadingVars.xyz);
#	endif

	return psout;
}
#endif

#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0-t7 and s0-s7 from the heap, the shared
// constants, and PerGeometry from the engine-constants capture through the pass's latch. The vertex stage is the graph's
// fullscreen triangle.
#	define ORG_SHARED_CONSTANTS
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISLIGHTINGCOMPOSITE_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, AlbedoTex, 0) X(SRV, Texture2D<float4>, DiffuseTex, 1) X(SRV, Texture2D<float4>, SpecularTex, 2) X(SRV, Texture2D<float4>, SAOTex, 3) X(SRV, Texture2D<float4>, FogTex, 4) X(SRV, Texture2D<float4>, DirDiffuseTex, 5) X(SRV, Texture2D<float4>, DirSpecularTex, 6) X(SRV, Texture2D<float4>, ShadowMaskTex, 7) X(SAMPLER, SamplerState, AlbedoSampler, 0) X(SAMPLER, SamplerState, DiffuseSampler, 1) X(SAMPLER, SamplerState, SpecularSampler, 2) X(SAMPLER, SamplerState, SAOSampler, 3) X(SAMPLER, SamplerState, FogSampler, 4) X(SAMPLER, SamplerState, DirDiffuseSampler, 5) X(SAMPLER, SamplerState, DirSpecularSampler, 6) X(SAMPLER, SamplerState, ShadowMaskSampler, 7) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISLIGHTINGCOMPOSITE_ORG_RESOURCES(ORG_HANDLE))
ISLIGHTINGCOMPOSITE_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/DummyVSTexCoord.hlsl"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
static float4 FogParam = OrgEngineRow(PerGeometryAt, 0);
static float4 FogNearColor = OrgEngineRow(PerGeometryAt, 1);
#	else
SamplerState AlbedoSampler : register(s0);
SamplerState DiffuseSampler : register(s1);
SamplerState SpecularSampler : register(s2);
SamplerState SAOSampler : register(s3);
SamplerState FogSampler : register(s4);
SamplerState DirDiffuseSampler : register(s5);
SamplerState DirSpecularSampler : register(s6);
SamplerState ShadowMaskSampler : register(s7);

Texture2D<float4> AlbedoTex : register(t0);
Texture2D<float4> DiffuseTex : register(t1);
Texture2D<float4> SpecularTex : register(t2);
Texture2D<float4> SAOTex : register(t3);
Texture2D<float4> FogTex : register(t4);
Texture2D<float4> DirDiffuseTex : register(t5);
Texture2D<float4> DirSpecularTex : register(t6);
Texture2D<float4> ShadowMaskTex : register(t7);

cbuffer PerGeometry : register(b2)
{
	float4 FogParam : packoffset(c0);
	float4 FogNearColor : packoffset(c1);
};
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float4 diffuse = DiffuseTex.Sample(DiffuseSampler, input.TexCoord);
	float4 specular = SpecularTex.Sample(SpecularSampler, input.TexCoord);
	float4 albedo = AlbedoTex.Sample(AlbedoSampler, input.TexCoord);

#	if defined(DIRECTIONALLIGHT)
	float4 dirDiffuse = DirDiffuseTex.Sample(DirDiffuseSampler, input.TexCoord);
	float4 dirSpecular = DirSpecularTex.Sample(DirSpecularSampler, input.TexCoord);
#	else
	float4 dirDiffuse = 0;
	float4 dirSpecular = 0;
#	endif

#	if !defined(MENU)
	float shadowMask = ShadowMaskTex.Sample(ShadowMaskSampler, input.TexCoord).x;
	float sao = SAOTex.Sample(SAOSampler, input.TexCoord).x;
#	else
	float shadowMask = 1;
	float sao = 1;
#	endif

	float4 preFog = (diffuse * sao + shadowMask * dirDiffuse) * albedo +
	                (specular * sao + dirSpecular * shadowMask);

	float4 fog = FogTex.Sample(FogSampler, input.TexCoord);

	if (fog.x + fog.y + fog.z + fog.w != 0) {
		psout.Color = float4(saturate(FogNearColor.w * lerp(preFog.xyz, fog.xyz, fog.w)), saturate(preFog.w));
	} else {
		psout.Color = preFog;
	}

	return psout;
}
#endif

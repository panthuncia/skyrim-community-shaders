#if defined(CS_ORG_BINDLESS)
// The ORG build (Features/VolumetricLighting/ORGVolumetricLighting): every resource from the heap through the push block. The
// shared constant buffers and the headers' resources (Terrain and Cloud Shadows, through ShadowSampling) are heap reads too
// (ORG_SHARED_CONSTANTS, ORG_HEADER_RESOURCES). PerTechnique is the engine's block, latched by the port, with the depth
// repartition's exponent after it: the inverse repartition texture is computed (Common/VolumetricLightingRepartition.hlsli).
#	define ORG_SHARED_CONSTANTS
#	define ORG_HEADER_RESOURCES
#	include "Common/OrgResources.hlsli"
#	include "Common/VolumetricLightingRepartition.hlsli"

// Rows 0-23: the engine's PerTechnique (SetComputeShaderConstants, 0x180 bytes). Row 24: x the inverse repartition's exponent.
struct VolumetricLightingGenerateConstants
{
	float4 Rows[25];
};

#	define VL_GENERATE_RESOURCES(X)                                            		X(SHARED, ConstantBuffer, Shared, 0)                                    		X(SAMPLER, SamplerState, ShadowmapSampler, 0)                           		X(SAMPLER, SamplerState, ShadowmapVLSampler, 1)                         		X(SAMPLER, SamplerState, NoiseSampler, 3)                               		X(SRV, Texture2DArray<float>, ShadowmapTex, 0)                          		X(SRV, Texture2DArray<float>, ShadowmapVLTex, 1)                        		X(SRV, Texture3D<float>, NoiseTex, 3)                                   		X(UAV, RWTexture3D<float>, DensityRW, 0)                                		X(LATCH, VolumetricLightingGenerateConstants, PerTechnique, 0)          		X(EXTERN, Texture2D<float2>, ShadowHeightTexture, 60)                   		X(EXTERN, TextureCube<float>, CloudShadowsTexture, 25)                  		X(UNREAD, Texture2D<float4>, DepthTexture, 17)                          		X(UNREAD, Texture2D<float2>, SharedShadowMap, 18)                       		X(UNREAD, Texture2D<float4>, EnvIBLTexture, 76)                         		X(UNREAD, Texture2D<float4>, SkyIBLTexture, 77)                         		X(UNREAD_LATCH, DirectionalShadowLightData, DirectionalShadowLight, 98)
ORG_PUSH_BLOCK(VL_GENERATE_RESOURCES(ORG_HANDLE))
VL_GENERATE_RESOURCES(ORG_RESOURCE)

static float4x4 CameraViewProj = float4x4(PerTechnique.Rows[0], PerTechnique.Rows[1], PerTechnique.Rows[2], PerTechnique.Rows[3]);
static float4x4 CameraViewProjInverse = float4x4(PerTechnique.Rows[4], PerTechnique.Rows[5], PerTechnique.Rows[6], PerTechnique.Rows[7]);
// Column-major in the engine's block: each cascade's three columns are three rows.
static float4x3 ShadowMapProj[3] = {
	transpose(float3x4(PerTechnique.Rows[8], PerTechnique.Rows[9], PerTechnique.Rows[10])),
	transpose(float3x4(PerTechnique.Rows[11], PerTechnique.Rows[12], PerTechnique.Rows[13])),
	transpose(float3x4(PerTechnique.Rows[14], PerTechnique.Rows[15], PerTechnique.Rows[16]))
};
static float3 EndSplitDistances = PerTechnique.Rows[17].xyz;
static float ShadowMapCount = PerTechnique.Rows[17].w;
static float EnableShadowCasting = PerTechnique.Rows[18].x;
static float3 DirLightDirection = PerTechnique.Rows[19].xyz;
static float3 TextureDimensions = PerTechnique.Rows[20].xyz;
static float3 WindInput = PerTechnique.Rows[21].xyz;
static float InverseDensityScale = PerTechnique.Rows[21].w;
static float3 PosAdjust = PerTechnique.Rows[22].xyz;
static float IterationIndex = PerTechnique.Rows[22].w;
static float PhaseContribution = PerTechnique.Rows[23].x;
static float PhaseScattering = PerTechnique.Rows[23].y;
static float DensityContribution = PerTechnique.Rows[23].z;

#	define VL_INVERSE_REPARTITION(U) VolumetricLightingRepartition::Sample(U, PerTechnique.Rows[24].x)
#endif

#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

#if defined(CSHADER)
#	if !defined(CS_ORG_BINDLESS)
SamplerState ShadowmapSampler : register(s0);
SamplerState ShadowmapVLSampler : register(s1);
SamplerState InverseRepartitionSampler : register(s2);
SamplerState NoiseSampler : register(s3);

Texture2DArray<float> ShadowmapTex : register(t0);
Texture2DArray<float> ShadowmapVLTex : register(t1);
Texture1D<float> InverseRepartitionTex : register(t2);
Texture3D<float> NoiseTex : register(t3);

RWTexture3D<float> DensityRW : register(u0);

#		define VL_INVERSE_REPARTITION(U) InverseRepartitionTex.SampleLevel(InverseRepartitionSampler, U, 0)
#	endif

#	define LinearSampler ShadowmapSampler

#	include "Common/Framebuffer.hlsli"
#	include "Common/SharedData.hlsli"

#	include "Common/ShadowSampling.hlsli"

#	if !defined(CS_ORG_BINDLESS)
cbuffer PerTechnique : register(b0)
{
	row_major float4x4 CameraViewProj : packoffset(c0);
	row_major float4x4 CameraViewProjInverse : packoffset(c4);
	float4x3 ShadowMapProj[3] : packoffset(c8);
	float3 EndSplitDistances : packoffset(c17.x);
	float ShadowMapCount : packoffset(c17.w);
	float EnableShadowCasting : packoffset(c18);
	float3 DirLightDirection : packoffset(c19);
	float3 TextureDimensions : packoffset(c20);
	float3 WindInput : packoffset(c21);
	float InverseDensityScale : packoffset(c21.w);
	float3 PosAdjust : packoffset(c22);
	float IterationIndex : packoffset(c22.w);
	float PhaseContribution : packoffset(c23.x);
	float PhaseScattering : packoffset(c23.y);
	float DensityContribution : packoffset(c23.z);
}
#	endif

[numthreads(32, 32, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const float3 StepCoefficients[] = {
		{ 0, 0, 0 },
		{ 0, 0, 0.001 },
		{ 0, 0.001, 0 },
		{ 0, 0.001, 0.001 },
		{ 0.001, 0, 0 },
		{ 0.001, 0, 0.001 },
		{ 0.001, 0.001, 0 },
		{ 0.001, 0.001, 0.001 }
	};

	float3 normalizedCoordinates = float3(dispatchID.xy + 0.5, dispatchID.z - 1.0) * rcp(TextureDimensions.xyz);
	float3 depthUv = normalizedCoordinates + StepCoefficients[IterationIndex];
	float depth = VL_INVERSE_REPARTITION(depthUv.z);
	float4 positionCS = float4(2 * depthUv.x - 1, 1 - 2 * depthUv.y, depth, 1);

	float4 positionWS = mul(CameraViewProjInverse, positionCS);
	positionWS *= rcp(positionWS.w);

	float4 positionCSShifted = mul(CameraViewProj, positionWS);
	positionCSShifted *= rcp(positionCSShifted.w);

	float shadowMapDepth = positionCSShifted.z;

	bool noShadow = !SharedData::InInterior;
	if (EndSplitDistances.z >= shadowMapDepth) {
		uint cascadeIndex = ShadowMapCount >= 3.0f && shadowMapDepth > EndSplitDistances.y ? 2 : shadowMapDepth > EndSplitDistances.x ? 1 :
		                                                                                                                                0;
		float shadowMapThreshold = cascadeIndex == 0 ? 0.01f : 0.0f;
		float4x3 lightProjectionMatrix = ShadowMapProj[cascadeIndex];

		float3 positionLS = mul(transpose(lightProjectionMatrix), float4(positionWS.xyz, 1)).xyz;
		float shadowMapValue = ShadowmapTex.SampleLevel(ShadowmapSampler, float3(positionLS.xy, cascadeIndex), 0);
		noShadow = shadowMapValue >= positionLS.z - shadowMapThreshold;

		if (EnableShadowCasting < 0.5) {
			float shadowMapVLValue = ShadowmapVLTex.SampleLevel(ShadowmapVLSampler, float3(positionLS.xy, cascadeIndex), 0);
			noShadow = noShadow & shadowMapVLValue >= positionLS.z - shadowMapThreshold;
		}
	}

	float3 noiseUv = 0.0125 * (InverseDensityScale * (positionWS.xyz + WindInput));
	float noise = NoiseTex.SampleLevel(NoiseSampler, noiseUv, 0);
	float densityFactor = noise * (1 - 0.75 * smoothstep(0, 1, saturate(2 * positionWS.z / 300)));
	float densityContribution = lerp(1, densityFactor, DensityContribution);

	float LdotN = dot(normalize(-positionWS.xyz), DirLightDirection);
	float phaseFactor = (1 - PhaseScattering * PhaseScattering) * rcp(4 * Math::PI * (1 - LdotN * PhaseScattering));
	float phaseContribution = lerp(1, phaseFactor, PhaseContribution);

	float shadowContribution = noShadow;

#	if defined(TERRAIN_SHADOWS) || defined(CLOUD_SHADOWS)
	shadowContribution *= sqrt(ShadowSampling::GetWorldShadow(positionWS.xyz, PosAdjust));
#	endif

	float vl = shadowContribution * densityContribution * phaseContribution;

	DensityRW[dispatchID.xyz] = vl;
}
#endif

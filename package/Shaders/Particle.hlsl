// SPDX-License-Identifier: MIT AND (LicenseRef-GPL-3.0-with-Modding-Exception WITH GPL-3.0-linking-source-exception)
// Original code Copyright (c) 2022 Ilya Perapechka (MIT), modifications GPL-3.0 with Modding Exception (see EXCEPTIONS.md)

#if defined(CS_ORG_BINDLESS)
// The ORG build (Drawcall Limit Fix's precipitation family, Features/DrawcallLimitFix/Features/Precipitation): the ENVCUBE techniques
// (rain, snow) drawn by the graph in the engine's place. One push block serves both stages, laid out as Water.hlsl's (the same headers
// and the same latches): a draw's engine constants (VS PerTechnique and PerGeometry, PS PerGeometry), its descriptors, its textures' and
// samplers' heap indices and its geometry are one latch region; Light Limit Fix's strict lights (PS b3) and the directional shadow record
// (t98) follow it. The shared constants and the headers' other resources come from the heap through the push block.
#	define ORG_SHARED_CONSTANTS
#	define ORG_HEADER_RESOURCES
// The permutation block is the draw's (ParticleOrgPermutation), not a shared buffer.
#	define DCLF_PULLED_ROWS
#	define SKYLIGHTING_PROBE_REGISTER t50
#	include "Common/OrgResources.hlsli"

// A draw's latched values, as the engine's SetupTechnique and SetupGeometry wrote its constant buffers (c rows in the D3D11 shaders'
// layout). Matrices are rows: a latch is loaded with Load<T>, which does not keep row_major. 592 bytes.
struct ParticleOrgDraw
{
	float4 VSTechnique[1];  // VS b0: ScaleAdjust
	float4 VSGeometry[23];  // VS b2: WorldViewProj, WorldView, PrecipitationOcclusionWorldViewProj (ENVCUBE), fVars0-4, Color1-3, Velocity, Acceleration, Wind
	float4 PSGeometry[2];   // PS b2: ColorScale, TextureSize
	// What the vertex stage reads of CS's shared constants, copied here: DXC 1.9's SPIR-V for a vertex stage that pulls its vertices
	// (vk::RawBufferLoad) and reads a shared view (a static ConstantBuffer) fails validation.
	float4 Projection;         // FrameBuffer: CameraProj[0][0], CameraProj[1][1], DynamicResolutionParams1.xy
	float4 BufferDim;          // SharedData
	float4 RainRefraction[3];  // FeatureData: Rain Refraction's RainRefractionSettings
	uint4 Descriptors;      // CS's permutation block: vertex, pixel, extra and extra feature descriptors
	float EffectRadius;
	uint VertexStride;
	uint FirstIndex;
	uint RaindropNormals;  // t80's heap index (Rain Refraction's raindrop normals)
	uint4 Textures;        // t0-t3 heap indices
	uint4 Samplers;        // s0-s3 sampler heap indices
	uint2 VertexAddress;
	uint2 IndexAddress;
	uint2 VertexLayout;
	uint2 Padding;
};

#	define PARTICLE_ORG_RESOURCES(X)                                                      \
		X(SHARED, ConstantBuffer, Shared, 0)                                                 \
		X(LATCH, ParticleOrgDraw, ParticleDraw, 2)                                           \
		X(EXTERN_LATCH, StrictLightDataValues, StrictLightData, 3)                           \
		X(EXTERN, Texture2D<float4>, DepthTexture, 17)                                       \
		X(EXTERN, Texture2D<float2>, SharedShadowMap, 18)                                    \
		X(EXTERN, Texture3D<float4>, ExponentialHeightFogIntegratedLightScattering, 19)      \
		X(EXTERN, Texture3D<float4>, ExponentialHeightFogIntegratedLightScatteringFar, 22)   \
		X(EXTERN, TextureCube<float>, CloudShadowsTexture, 25)                               \
		X(EXTERN, TextureCube<float>, CloudSelfShadowTexture, 26)                            \
		X(EXTERN, TextureCube<float3>, EnvReflectionsTexture, 30)                            \
		X(EXTERN, TextureCube<float3>, EnvTexture, 31)                                       \
		X(EXTERN, StructuredBuffer<Light>, lights, 35)                                       \
		X(EXTERN, StructuredBuffer<uint>, lightList, 36)                                     \
		X(EXTERN, StructuredBuffer<LightGrid>, lightGrid, 37)                                \
		X(EXTERN, Texture3D<sh2>, SkylightingProbeArray, 50)                                 \
		X(EXTERN, Texture2D<float2>, ShadowHeightTexture, 60)                                \
		X(EXTERN, Texture2D<float4>, TexPrecipOcclusion, 70)                                 \
		X(EXTERN, Texture2D<sh2>, EnvIBLTexture, 76)                                         \
		X(EXTERN, Texture2D<sh2>, SkyIBLTexture, 77)                                         \
		X(EXTERN_LATCH, DirectionalShadowLightData, DirectionalShadowLight, 98)              \
		X(EXTERN, StructuredBuffer<LocalShadowData>, LocalShadows, 102)                      \
		X(EXTERN, Texture2DArray<float>, LocalShadowMaps, 103)
ORG_PUSH_BLOCK(PARTICLE_ORG_RESOURCES(ORG_HANDLE))
PARTICLE_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/SharedData.hlsli"

#if !defined(DYNAMIC_CUBEMAPS) && defined(IBL)
#	undef IBL
#endif

#if defined(ENVCUBE) && defined(RAIN) && defined(RAIN_REFRACTION)
namespace Raindrop
{
	static const float Radius = 0.003 / GAME_UNIT_TO_M;
	static const float MinimumRadiusPixels = 1.0;  // Smaller drops widen to this and fade by the area they gained
}
#endif

#if defined(VSHADER)
struct VS_INPUT
{
	float4 Position: POSITION0;
#	if !defined(ENVCUBE)
	float4 Normal: NORMAL0;
#	endif
	float4 TexCoord0: TEXCOORD0;
#	if defined(ENVCUBE)
	float4
#	else
	int4
#	endif
		TexCoord1: TEXCOORD1;
};
#endif

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
	float4 Color: COLOR0;
	float2 TexCoord0: TEXCOORD0;
#if defined(ENVCUBE)
	float4 PrecipitationOcclusionTexCoord: TEXCOORD1;
#endif
#if defined(ENVCUBE) && defined(RAIN) && defined(RAIN_REFRACTION)
	float3 RaindropData: TEXCOORD2;  // xy: raindrop texture UV, z: coverage left after widening to the minimum size
#endif
};

#ifdef VSHADER
#	if defined(CS_ORG_BINDLESS)
static float2 ScaleAdjust = ParticleDraw.VSTechnique[0].xy;
static float4x4 WorldViewProj = float4x4(ParticleDraw.VSGeometry[0], ParticleDraw.VSGeometry[1], ParticleDraw.VSGeometry[2], ParticleDraw.VSGeometry[3]);
static float4x4 WorldView = float4x4(ParticleDraw.VSGeometry[4], ParticleDraw.VSGeometry[5], ParticleDraw.VSGeometry[6], ParticleDraw.VSGeometry[7]);
#		if defined(ENVCUBE)
static float4x4 PrecipitationOcclusionWorldViewProj =
	float4x4(ParticleDraw.VSGeometry[8], ParticleDraw.VSGeometry[9], ParticleDraw.VSGeometry[10], ParticleDraw.VSGeometry[11]);
// The rows after the matrices: four further down with ENVCUBE's occlusion matrix.
#			define PARTICLE_ORG_ROW(N) ParticleDraw.VSGeometry[(N) + 12]
#		else
#			define PARTICLE_ORG_ROW(N) ParticleDraw.VSGeometry[(N) + 8]
#		endif
static float4 fVars0 = PARTICLE_ORG_ROW(0);
static float4 fVars1 = PARTICLE_ORG_ROW(1);
static float4 fVars2 = PARTICLE_ORG_ROW(2);
static float4 fVars3 = PARTICLE_ORG_ROW(3);
static float4 fVars4 = PARTICLE_ORG_ROW(4);
static float4 Color1 = PARTICLE_ORG_ROW(5);
static float4 Color2 = PARTICLE_ORG_ROW(6);
static float4 Color3 = PARTICLE_ORG_ROW(7);
static float4 Velocity = PARTICLE_ORG_ROW(8);
static float4 Acceleration = PARTICLE_ORG_ROW(9);
static float4 Wind = PARTICLE_ORG_ROW(10);

/** @brief Rain Refraction's settings as the draw's latch holds them (ParticleOrgDraw.RainRefraction). */
SharedData::RainRefractionSettings ParticleOrgRainRefraction()
{
	SharedData::RainRefractionSettings settings = (SharedData::RainRefractionSettings)0;
	settings.Enable = asuint(ParticleDraw.RainRefraction[0].x);
	settings.MotionStretch = ParticleDraw.RainRefraction[0].y;
	settings.MotionTransparency = ParticleDraw.RainRefraction[0].z;
	settings.TextureBounds = ParticleDraw.RainRefraction[1];
	settings.TextureRadiusScale = ParticleDraw.RainRefraction[2].xy;
	return settings;
}
static SharedData::RainRefractionSettings ParticleRainRefraction = ParticleOrgRainRefraction();
static float4 ParticleBufferDim = ParticleDraw.BufferDim;
static float2 ParticleProjectionScale = ParticleDraw.Projection.xy;
static float2 ParticleDynamicResolution = ParticleDraw.Projection.zw;
#	else
#		define ParticleProjectionScale float2(FrameBuffer::CameraProj[0][0], FrameBuffer::CameraProj[1][1])
#		define ParticleDynamicResolution FrameBuffer::DynamicResolutionParams1.xy
#		define ParticleRainRefraction SharedData::rainRefractionSettings
#		define ParticleBufferDim SharedData::BufferDim
cbuffer PerTechnique : register(b0)
{
	float2 ScaleAdjust : packoffset(c0);
};

cbuffer PerGeometry : register(b2)
{
	row_major float4x4 WorldViewProj;  // 0
	row_major float4x4 WorldView;      // 4
#	if defined(ENVCUBE)
	row_major float4x4 PrecipitationOcclusionWorldViewProj;  // 8, 16
#	endif
	float4 fVars0;        // 8, 16 ENVCUBE 12, 20
	float4 fVars1;        // 9, 17 ENVCUBE 13, 21
	float4 fVars2;        // 10, 18 ENVCUBE 14, 22
	float4 fVars3;        // 11, 19 ENVCUBE 15, 23
	float4 fVars4;        // 12, 20 ENVCUBE 16, 24
	float4 Color1;        // 13, 21 ENVCUBE 17, 25
	float4 Color2;        // 14, 22 ENVCUBE 18, 26
	float4 Color3;        // 15, 23 ENVCUBE 19, 27
	float4 Velocity;      // 16, 24 ENVCUBE 20, 28
	float4 Acceleration;  // 17, 25 ENVCUBE 21, 29
	float4 Wind;          // 18, 26 ENVCUBE 22, 30
}
#	endif

float2x2 GetRotationMatrix(float angle)
{
	float sine, cosine;
	sincos(angle, sine, cosine);

	return float2x2(float2(cosine, -sine), float2(sine, cosine));
}

#	if defined(CS_ORG_BINDLESS)
VS_OUTPUT ParticleVertex(VS_INPUT input)
#	else
VS_OUTPUT main(VS_INPUT input)
#	endif
{
	VS_OUTPUT vsout;

#	if defined(ENVCUBE)
#		if defined(RAIN)
	float2 positionOffset = input.TexCoord1.xy;
#		else
	float2x2 rotationMatrix = GetRotationMatrix(fVars0.w);
	float2 positionOffset = mul(rotationMatrix, input.TexCoord1.xy) * ScaleAdjust + mul(rotationMatrix, input.TexCoord1.zw);
#		endif

	float3 normalizedPosition = (fVars0.xyz + input.Position.xyz) / fVars2.xxx;
	normalizedPosition = normalizedPosition >= -normalizedPosition ? frac(abs(normalizedPosition)) :
	                                                                 -frac(abs(normalizedPosition));

	float4 msPosition;
	msPosition.xyz = normalizedPosition * fVars2.xxx + (-(fVars2.x * 0.5).xxx + fVars1.xyz);
	msPosition.w = 1;

#		if defined(RAIN) && defined(RAIN_REFRACTION)
	vsout.RaindropData.z = 1.0;
	if (ParticleRainRefraction.Enable) {
		// Physically sized drop, centered on the vanilla streak and stretched along its motion
		float2 corner = sign(input.TexCoord1.xy);
		float3 motion = Velocity.xyz * abs(input.TexCoord1.y) * ParticleRainRefraction.MotionStretch;
		float4 center = msPosition - float4(Velocity.xyz * 0.5, 0);
		float4 leading = mul(WorldViewProj, center + float4(motion * 0.5, 0));
		float4 trailing = mul(WorldViewProj, center - float4(motion * 0.5, 0));
		float4 dropPosition = lerp(trailing, leading, corner.y * 0.5 + 0.5);

		float2 renderSize = ParticleBufferDim.xy * ParticleDynamicResolution;
		float2 radiusClip = Raindrop::Radius * ParticleRainRefraction.TextureRadiusScale * abs(ParticleProjectionScale);
		float2 radiusPixels = radiusClip * renderSize * 0.5 / max(abs(dropPosition.w), EPSILON_DIVISION);
		float rasterScale = max(1.0, Raindrop::MinimumRadiusPixels / max(min(radiusPixels.x, radiusPixels.y), EPSILON_DIVISION));
		float2 motionPixels = (leading.xy / max(abs(leading.w), EPSILON_DIVISION) - trailing.xy / max(abs(trailing.w), EPSILON_DIVISION)) * renderSize * 0.5;

		// Fade by the ratio of the true drop's area to the widened one's, so sub-pixel drops keep their brightness
		float radius = sqrt(radiusPixels.x * radiusPixels.y);
		float rasterRadius = radius * rasterScale;
		float motionLength = length(motionPixels);
		float dropArea = Math::PI * radius * radius + 2.0 * radius * motionLength;
		float rasterArea = Math::PI * rasterRadius * rasterRadius + 2.0 * rasterRadius * motionLength;
		vsout.RaindropData.z = saturate(dropArea / max(rasterArea, EPSILON_DIVISION));

		vsout.Position = dropPosition;
		vsout.Position.xy += corner * radiusClip * rasterScale;
	} else
#		endif
	{
		float4 viewPosition = mul(WorldViewProj, msPosition);
#		if defined(RAIN)
		float4 adjustedMsPosition = msPosition - float4(Velocity.xyz, 0);
		float positionBlendParam = 0.5 * (1 + input.TexCoord1.y);
		float4 adjustedViewPosition = mul(WorldViewProj, adjustedMsPosition);
		float4 finalViewPosition = lerp(adjustedViewPosition, viewPosition, positionBlendParam);
#		else
		float4 finalViewPosition = viewPosition;
#		endif
		vsout.Position.xy = positionOffset + finalViewPosition.xy;
		vsout.Position.zw = finalViewPosition.zw;
	}
	vsout.Color.xyz = 1.0.xxx;
	vsout.Color.w = fVars1.w;

	vsout.TexCoord0.xy = input.TexCoord0.xy;

	float4 precipitationOcclusionTexCoord = mul(PrecipitationOcclusionWorldViewProj, msPosition);
	precipitationOcclusionTexCoord.y = -precipitationOcclusionTexCoord.y;
	vsout.PrecipitationOcclusionTexCoord = precipitationOcclusionTexCoord;
#	else
	float tmp2 = input.Normal.w * input.Position.w;
	float tmp1 = tmp2 / fVars0.y;

	float uvScale1, uvScale2, tmp3, tmp4;
	if (tmp1 > fVars2.w) {
		uvScale1 = fVars2.y;
		uvScale2 = 0;
		tmp3 = fVars2.w;
		tmp4 = 1;
	} else if (tmp1 > fVars2.z) {
		uvScale1 = fVars2.x;
		uvScale2 = fVars2.y;
		tmp3 = fVars2.z;
		tmp4 = fVars2.w;
	} else {
		uvScale1 = 0;
		uvScale2 = fVars2.x;
		tmp3 = 0;
		tmp4 = fVars2.z;
	}
	float uvScaleParam = (tmp1 - tmp3) / (tmp4 - tmp3);
	float uvScale = lerp(uvScale1, uvScale2, uvScaleParam);

	vsout.TexCoord0.xy = fVars4.xy * input.TexCoord1.xy;

	float2 uv1 = (input.TexCoord1.zw * 2.0.xx - 1.0.xx) * uvScale;
	float uvAngle = input.TexCoord0.y * input.Position.w + input.TexCoord0.x;
	float2x2 rotationMatrix = GetRotationMatrix(uvAngle);
	float2 positionOffset = mul(rotationMatrix, uv1);

	float4 msPosition;
	msPosition.xyz = -fVars3.xyz +
	                 (((input.Normal.xyz * fVars0.www + Acceleration.xyz) * (tmp2 * tmp2)) * 0.5 +
						 (((fVars0.zzz * input.Normal.xyz) * input.TexCoord0.zzz +
							  (normalize(-Wind.xyz + input.Position.xyz) * Wind.www + Velocity.xyz)) *
								 tmp2 +
							 input.Position.xyz));
	msPosition.w = 1;

	float4 viewPosition = mul(WorldViewProj, msPosition);
	vsout.Position.xy = positionOffset * ScaleAdjust + viewPosition.xy;
	vsout.Position.zw = viewPosition.zw;

	float4 color1, color2;
	float colorTmp1, colorTmp2;
	if (tmp1 > fVars1.z) {
		color1 = Color3.xyzw;
		color2 = float4(Color3.xyz, 0);
		colorTmp1 = fVars1.z;
		colorTmp2 = 1;
	} else if (tmp1 > fVars1.y) {
		color1 = Color2.xyzw;
		color2 = Color3.xyzw;
		colorTmp1 = fVars1.y;
		colorTmp2 = fVars1.z;
	} else if (tmp1 > fVars1.x) {
		color1 = Color1.xyzw;
		color2 = Color2.xyzw;
		colorTmp1 = fVars1.x;
		colorTmp2 = fVars1.y;
	} else {
		color1 = float4(Color1.xyz, 0);
		color2 = Color1.xyzw;
		colorTmp1 = 0;
		colorTmp2 = fVars1.x;
	}
#		if !defined(GRAYSCALE_TO_COLOR)
	color1.rgb = ColorManagement::SRGBToWorking(color1.rgb);
	color2.rgb = ColorManagement::SRGBToWorking(color2.rgb);
#		endif
	float colorParam = (tmp1 - colorTmp1) / (colorTmp2 - colorTmp1);
	float4 color = lerp(color1, color2, colorParam);

	vsout.Color.w = fVars3.w * color.w;
	vsout.Color.xyz = color.xyz;
#	endif

#	if defined(ENVCUBE) && defined(RAIN) && defined(RAIN_REFRACTION)
	// Corner offsets are +-particle size; their sign picks the corner of the drop's covered area in the raindrop texture, so one drop fills each particle
	vsout.RaindropData.xy = lerp(ParticleRainRefraction.TextureBounds.xy, ParticleRainRefraction.TextureBounds.zw, step(0, input.TexCoord1.xy));
#	endif

	return vsout;
}

#	if defined(CS_ORG_BINDLESS)
#		if defined(__spirv__)
#			define DCLF_PULLED_VERTICES
#			include "Common/DCLFVertexPull.hlsli"

/** @brief The draw's vertex a_vertexID, pulled as the input assembler reads it: its 16-bit index, then its attributes. */
VS_INPUT ParticlePullVertex(uint a_vertexID)
{
	DCLFDrawVertexLayout = ParticleDraw.VertexLayout;
	const uint indexNumber = ParticleDraw.FirstIndex + a_vertexID;
	const uint indexWord = DeviceLoad(uint, DCLFAddress(ParticleDraw.IndexAddress) + uint64_t((indexNumber * 2) & ~3u));
	const uint index = (indexNumber & 1) ? (indexWord >> 16) : (indexWord & 0xFFFF);
	DCLFStream first = { DCLFAddress(ParticleDraw.VertexAddress), ParticleDraw.VertexStride };
	DCLFStream second = { uint64_t(0), 0u };
	DCLFStreamsAt(index, first, second);

	VS_INPUT input = (VS_INPUT)0;
	input.Position = DCLFPullPosition(first, second);
	// TEXCOORD0 is R16G16 (the input assembler gives z 0 and w 1), TEXCOORD1 R16G16B16A16 (VertexInput.cpp, BuildVertexElements).
	input.TexCoord0 = float4(DCLFPullHalf2(1, first, second), 0, 1);
#				if defined(ENVCUBE)
	input.TexCoord1 = DCLFPullHalf4(2, first, second);
#				else
	input.Normal = DCLFPullUnorm4(3, 0, first, second);
	input.TexCoord1 = (int4)DCLFPullHalf4(2, first, second);
#				endif
	return input;
}
#		else
// The D3D12 build has no vertex pulling yet: it compiles, and draws nothing.
VS_INPUT ParticlePullVertex(uint a_vertexID)
{
	VS_INPUT input = (VS_INPUT)0;
	return input;
}
#		endif

VS_OUTPUT main(uint a_vertexID: SV_VertexID)
{
	return ParticleVertex(ParticlePullVertex(a_vertexID));
}
#	endif
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
	float4 Normal: SV_Target1;
};

#ifdef PSHADER

#	if defined(LIGHT_LIMIT_FIX)
#		include "LightLimitFix/LightLimitFix.hlsli"
#	endif

#	if defined(CS_ORG_BINDLESS)
static SamplerState SampSourceTexture = SamplerDescriptorHeap[ParticleDraw.Samplers.x];
#		if defined(GRAYSCALE_TO_COLOR) || defined(GRAYSCALE_TO_ALPHA)
static SamplerState SampGrayscaleTexture = SamplerDescriptorHeap[ParticleDraw.Samplers.y];
#		endif
#		if defined(ENVCUBE)
static SamplerState SampPrecipitationOcclusionTexture = SamplerDescriptorHeap[ParticleDraw.Samplers.z];
static SamplerState SampUnderwaterMask = SamplerDescriptorHeap[ParticleDraw.Samplers.w];
#		endif

static Texture2D<float4> TexSourceTexture = ResourceDescriptorHeap[ParticleDraw.Textures.x];
#		if defined(GRAYSCALE_TO_COLOR) || defined(GRAYSCALE_TO_ALPHA)
static Texture2D<float4> TexGrayscaleTexture = ResourceDescriptorHeap[ParticleDraw.Textures.y];
#		endif
#		if defined(ENVCUBE)
static Texture2D<float4> TexPrecipitationOcclusionTexture = ResourceDescriptorHeap[ParticleDraw.Textures.z];
static Texture2D<float4> TexUnderwaterMask = ResourceDescriptorHeap[ParticleDraw.Textures.w];
#		endif
#		if defined(ENVCUBE) && defined(RAIN) && defined(RAIN_REFRACTION)
static Texture2D<float4> TexRaindropNormals = ResourceDescriptorHeap[ParticleDraw.RaindropNormals];
#		endif

static float ColorScale = ParticleDraw.PSGeometry[0].x;
static float3 TextureSize = ParticleDraw.PSGeometry[1].xyz;
#	else
SamplerState SampSourceTexture : register(s0);
#	if defined(GRAYSCALE_TO_COLOR) || defined(GRAYSCALE_TO_ALPHA)
SamplerState SampGrayscaleTexture : register(s1);
#	endif
#	if defined(ENVCUBE)
SamplerState SampPrecipitationOcclusionTexture : register(s2);
SamplerState SampUnderwaterMask : register(s3);
#	endif

Texture2D<float4> TexSourceTexture : register(t0);
#	if defined(GRAYSCALE_TO_COLOR) || defined(GRAYSCALE_TO_ALPHA)
Texture2D<float4> TexGrayscaleTexture : register(t1);
#	endif
#	if defined(ENVCUBE)
Texture2D<float4> TexPrecipitationOcclusionTexture : register(t2);
Texture2D<float4> TexUnderwaterMask : register(t3);
#	endif
#	if defined(ENVCUBE) && defined(RAIN) && defined(RAIN_REFRACTION)
Texture2D<float4> TexRaindropNormals : register(t80);
#	endif

cbuffer PerGeometry : register(b2)
{
	float ColorScale : packoffset(c0);
	float3 TextureSize : packoffset(c1);
};
#	endif

#	define LinearSampler SampSourceTexture
#	include "Common/Permutation.hlsli"
#	include "Common/ShadowSampling.hlsli"

#	if defined(IBL)
#		include "IBL/IBL.hlsli"
#	endif

#	if defined(CS_ORG_BINDLESS)
/** @brief The draw's permutation block (Permutation.hlsli, DCLF_PULLED_ROWS), set by main before anything reads it. */
void ParticleOrgPermutation()
{
	Permutation::VertexShaderDescriptor = ParticleDraw.Descriptors.x;
	Permutation::PixelShaderDescriptor = ParticleDraw.Descriptors.y;
	Permutation::ExtraShaderDescriptor = ParticleDraw.Descriptors.z;
	Permutation::ExtraFeatureDescriptor = ParticleDraw.Descriptors.w;
	Permutation::EffectRadius = ParticleDraw.EffectRadius;
}
#	endif

#	if defined(DYNAMIC_CUBEMAPS)
#		define SampColorSampler SampSourceTexture
#		include "DynamicCubemaps/DynamicCubemaps.hlsli"
#	endif

#	if defined(ENVCUBE) && defined(RAIN) && defined(DYNAMIC_CUBEMAPS) && defined(RAIN_REFRACTION)
#		define REFRACTIVE_RAIN
#		include "Common/BRDF.hlsli"

namespace Raindrop
{
	static const float WaterF0 = 0.02;
	static const float Roughness = 0.3;              // Perceptual roughness of the drop's surface highlight
	static const float TransmissionExponent = 32.0;  // Tightness of the glow of a light seen through a drop
}
#	endif

/** @brief What a light shades: an unlit particle that takes light as-is, or a raindrop surface. */
struct ParticleSurface
{
	bool isRaindrop;
	float3 normal;
	float3 view;
	float3 refractDir;  ///< View ray continued through the drop
};

/** @brief Light a particle sends toward the viewer from lightColor arriving along L (toward the light). */
float3 ShadeLight(ParticleSurface surface, float3 lightColor, float3 L)
{
#	if defined(REFRACTIVE_RAIN)
	if (surface.isRaindrop) {
		float3 H = normalize(surface.view + L);
		float NdotL = saturate(dot(surface.normal, L));
		float NdotV = saturate(dot(surface.normal, surface.view));
		float NdotH = saturate(dot(surface.normal, H));
		float VdotH = saturate(dot(surface.view, H));
		float3 specular = BRDF::D_GGX(Raindrop::Roughness, NdotH) * BRDF::Vis_SmithJointApprox(Raindrop::Roughness, NdotV, NdotL) *
		                  BRDF::F_Schlick(Raindrop::WaterF0, VdotH) * NdotL;

		// A light behind the drop is seen through it along the refracted view ray; normalized Phong lobe
		float transmissionLobe = pow(saturate(dot(surface.refractDir, L)), Raindrop::TransmissionExponent) * (Raindrop::TransmissionExponent + 2.0) / Math::TAU;
		float3 transmission = transmissionLobe * (1.0 - BRDF::F_Schlick(Raindrop::WaterF0, NdotV));

		return ColorManagement::SceneToLinear(lightColor) * (specular + transmission);
	}
#	endif
	return lightColor;
}

/** @brief Sun color at positionWS after directional, world and cloud shadows. */
float3 GetDirLightColor(float3 positionWS)
{
	float unusedDetailedShadow;
	float3 dirLightColor = SharedData::DirLightColor.xyz * ShadowSampling::GetLightingShadow(positionWS, unusedDetailedShadow);
	// Menu and reflection passes don't match the world heightmap and cloud state
	if (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld)
		dirLightColor *= ShadowSampling::GetWorldShadow(positionWS, FrameBuffer::CameraPosAdjust.xyz);
	return dirLightColor;
}

/** @brief Adds the clustered point lights at positionWS, each shaded by ShadeLight, to lighting in light order. */
void AddPointLighting(float3 positionWS, ParticleSurface surface, inout float3 lighting)
{
#	if defined(LIGHT_LIMIT_FIX)
	float3 viewPosition = FrameBuffer::WorldToView(positionWS);
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition);

	uint clusterIndex = 0;
	if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		uint lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
		uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
		[loop] for (uint i = 0; i < lightCount; i++)
		{
			uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + i];
			LightLimitFix::Light light = LightLimitFix::lights[clusteredLightIndex];
			const bool localShadow = light.lightFlags & LightLimitFix::LightFlags::LocalShadow;
			if (LightLimitFix::IsLightIgnored(light) || ((light.lightFlags & LightLimitFix::LightFlags::ShadowCaster) && !localShadow)) {
				continue;
			}
			float3 lightDirection = light.positionWS.xyz - positionWS;
			float lightDist = length(lightDirection);
			lightDirection /= max(lightDist, 1e-5);

			float intensityMultiplier = LightLimitFix::GetAttenuation(lightDist, light) * (SharedData::lightLimitFixSettings.EnableLocalShadows ? light.fade : 1.0);
			[branch] if (intensityMultiplier > 1e-5 && localShadow)
				intensityMultiplier *= LightLimitFix::GetLocalShadow(LinearSampler, light.localShadowIndex, positionWS, FrameBuffer::CameraPosAdjust.xyz, lightDirection, false, 0.0);

			lighting += ShadeLight(surface, light.color.xyz * intensityMultiplier, lightDirection);
		}
	}
#	endif
}

#	if defined(CS_ORG_BINDLESS)
PS_OUTPUT ParticlePixel(PS_INPUT input, bool frontFace)
#	else
PS_OUTPUT main(PS_INPUT input, bool frontFace : SV_IsFrontFace)
#	endif
{
	PS_OUTPUT psout;

#	if defined(ENVCUBE)
	float2 precipitationOcclusionUV = (input.PrecipitationOcclusionTexCoord.xy * 0.5 + 0.5) * TextureSize.x;
	float precipitationOcclusion = -input.PrecipitationOcclusionTexCoord.z + TexPrecipitationOcclusionTexture.Load(float3(precipitationOcclusionUV, 0)).x;
	float2 underwaterMaskUv = TextureSize.yz * input.Position.xy;
	float underwaterMask = TexUnderwaterMask.Sample(SampUnderwaterMask, underwaterMaskUv).x;
	if (precipitationOcclusion - underwaterMask < 0) {
		discard;
	}
#	endif

#	if defined(REFRACTIVE_RAIN)
	if (SharedData::rainRefractionSettings.Enable) {
		float4 raindropNormal = TexRaindropNormals.Sample(SampSourceTexture, input.RaindropData.xy);
		float alpha = saturate(raindropNormal.w * input.RaindropData.z * (1.0 - SharedData::rainRefractionSettings.MotionTransparency));
		raindropNormal.y = 1.0 - raindropNormal.y;

		// Reconstruct camera-relative worldspace position (camera at origin).
		float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(input.Position.xy * SharedData::BufferDim.zw);
		float4 posCS = float4(2.0 * float2(uv.x, 1.0 - uv.y) - 1.0, input.Position.z, 1.0);
		float4 posWS = mul(FrameBuffer::CameraViewProjInverse, posCS);
		posWS.xyz /= posWS.w;

		// Build worldspace TBN from screen-space derivatives. The billboard is camera-aligned,
		// so dPdx/dPdy lie in the billboard plane along screen X/Y.
		float3 T = normalize(ddx(posWS.xyz));
		float3 N = normalize(cross(T, -ddy(posWS.xyz)));
		float3 B = cross(N, T);
		float3x3 TBN = float3x3(T, B, N);

		float3 normalTS = normalize(raindropNormal.xyz * 2.0 - 1.0);
		float3 normalWS = normalize(mul(normalTS, TBN));

		// Antialias the edges of the drop's covered area, which the billboard now ends at
		float2 dropUV = (input.RaindropData.xy - SharedData::rainRefractionSettings.TextureBounds.xy) /
		                (SharedData::rainRefractionSettings.TextureBounds.zw - SharedData::rainRefractionSettings.TextureBounds.xy);
		float2 edgeCoverage = saturate(min(dropUV, 1.0 - dropUV) / max(fwidth(dropUV), EPSILON_DIVISION));
		alpha *= edgeCoverage.x * edgeCoverage.y;
		if (alpha <= 0.0)
			discard;

		if (frontFace)
			normalWS = -normalWS;

		float3 V = normalize(-posWS.xyz);
		float NdotV = saturate(dot(normalWS, V));
		float fresnel = 0.02 + 0.98 * pow(1.0 - NdotV, 5.0);

		float3 reflectDir = reflect(-V, normalWS);
		float3 refractDir = refract(-V, normalWS, 1.0 / 1.33);
		if (dot(refractDir, refractDir) < 1e-4)
			refractDir = -V;

		float3 reflectColor = ColorManagement::SceneToLinear(DynamicCubemaps::EnvReflectionsTexture.SampleLevel(SampSourceTexture, reflectDir, 0).xyz);
		float3 refractColor = ColorManagement::SceneToLinear(DynamicCubemaps::EnvReflectionsTexture.SampleLevel(SampSourceTexture, refractDir, 0).xyz);

		// The env cubemap only carries faint bounce light, so lights are shaded on the drop directly
		ParticleSurface drop = { true, normalWS, V, refractDir };
		float3 directLighting = ShadeLight(drop, GetDirLightColor(posWS.xyz), SharedData::DirLightDirection.xyz);
		AddPointLighting(posWS.xyz, drop, directLighting);

		psout.Color.xyz = ColorManagement::LinearToScene(lerp(refractColor, reflectColor, fresnel) + directLighting);
		psout.Color.w = alpha;
		psout.Normal = float4(0, 1, 0, alpha);
		return psout;
	}
#	endif

	float4 sourceColor = TexSourceTexture.Sample(SampSourceTexture, input.TexCoord0);
	float4 baseColor;
#	if defined(GRAYSCALE_TO_COLOR)
	baseColor = input.Color * sourceColor;
#	else
	baseColor.xyz = input.Color.xyz * Color::Albedo(ColorManagement::TextureToWorking(sourceColor.xyz));
	baseColor.w = input.Color.w * sourceColor.w;
#	endif
#	if defined(GRAYSCALE_TO_COLOR)
	float3 grayScaleColor =
		TexGrayscaleTexture.Sample(SampGrayscaleTexture, float2(sourceColor.y, input.Color.x)).xyz;
	baseColor.xyz = ColorManagement::TextureToWorking(grayScaleColor);
#	endif
#	if defined(GRAYSCALE_TO_ALPHA)
	float grayScaleAlpha =
		TexGrayscaleTexture.Sample(SampGrayscaleTexture, float2(sourceColor.w, input.Color.w)).w;
	baseColor.w = grayScaleAlpha;
#	endif

	float3 propertyColor = 0.0;

	float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(input.Position.xy * SharedData::BufferDim.zw);

	float4 positionWS = float4(2 * float2(uv.x, -uv.y + 1) - 1, input.Position.z, 1);
	positionWS = mul(FrameBuffer::CameraViewProjInverse, positionWS);
	positionWS.xyz = positionWS.xyz / positionWS.w;

	float3 dirLightColor = GetDirLightColor(positionWS.xyz);
	float3 ambientColor = max(0, SharedData::GetAmbient(float3(0, 0, 1)));
#	if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		ambientColor = ImageBasedLighting::GetDiffuseIBL(ambientColor, float3(0, 0, -1));
	}
#	endif

	const ParticleSurface unlit = (ParticleSurface)0;
	propertyColor += dirLightColor;
	propertyColor += ambientColor;
	AddPointLighting(positionWS.xyz, unlit, propertyColor);

	psout.Color.xyz = propertyColor * baseColor.xyz;
	psout.Color.w = baseColor.w;
	psout.Normal.w = baseColor.w;
	psout.Normal.xyz = float3(0, 1, 0);

#	if defined(ENABLE_LL)
	if (!Permutation::RenderToUI || (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection)) {
#		if !defined(ENVCUBE)
		if (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::SourceAlphaBlend)
#		endif
			psout.Color.w = pow(saturate(psout.Color.w), TransferFunctions::GAME_GAMMA);
	}
	if (Permutation::RenderToUI && !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection))
		psout.Color.rgb = ColorManagement::WorkingToUI(psout.Color.rgb);
#	endif
	return psout;
}
#	if defined(CS_ORG_BINDLESS)
PS_OUTPUT main(PS_INPUT input, bool frontFace : SV_IsFrontFace)
{
	ParticleOrgPermutation();
	return ParticlePixel(input, frontFace);
}
#	endif
#endif

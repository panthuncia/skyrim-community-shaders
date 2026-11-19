#ifndef PHYSICAL_SKY_AP_SHADOW_HLSLI
#define PHYSICAL_SKY_AP_SHADOW_HLSLI

#ifndef AP_DIRECT_SHADOW_REGISTER
#	define AP_DIRECT_SHADOW_REGISTER t1
#endif
#ifndef TERRAIN_SHADOW_REGISTER
#	define TERRAIN_SHADOW_REGISTER t3
#endif
#include "CloudShadows/CloudShadows.hlsli"
#include "TerrainShadows/TerrainShadows.hlsli"

namespace AtmosphereShadow
{
	struct DirectionalShadowLightData
	{
		column_major float4x4 ShadowProj[2];
		column_major float4x4 InvShadowProj[2];
		float2 EndSplitDistances;
		float2 StartSplitDistances;
	};

#if defined(CS_ORG_BINDLESS) && defined(ORG_HEADER_RESOURCES)
	// ORG: the pass lists TexDirectShadows (EXTERN) and ApShadowLight (EXTERN_LATCH). The latch holds the light,
	// then the light count (0 or 1) and a uint the pass may use.
	static Texture2DArray<float4> TexDirectShadows = ResourceDescriptorHeap[OrgHandle_TexDirectShadows];
	static ByteAddressBuffer ShadowLightLatch = ResourceDescriptorHeap[OrgHandle_ApShadowLight];
	static const uint kShadowLightBytes = 272;

	uint ShadowLightCount() { return ShadowLightLatch.Load(OrgOffset_ApShadowLight + kShadowLightBytes); }

	// The bytes are those D3D11's structured buffer holds: each column_major matrix is four columns in a row.
	float4x4 LoadColumnMajor(uint a_offset)
	{
		const float4x4 columns = float4x4(asfloat(ShadowLightLatch.Load4(a_offset)), asfloat(ShadowLightLatch.Load4(a_offset + 16)),
			asfloat(ShadowLightLatch.Load4(a_offset + 32)), asfloat(ShadowLightLatch.Load4(a_offset + 48)));
		return transpose(columns);
	}

	DirectionalShadowLightData ShadowLight()
	{
		const uint base = OrgOffset_ApShadowLight;
		DirectionalShadowLightData light;
		light.ShadowProj[0] = LoadColumnMajor(base);
		light.ShadowProj[1] = LoadColumnMajor(base + 64);
		light.InvShadowProj[0] = LoadColumnMajor(base + 128);
		light.InvShadowProj[1] = LoadColumnMajor(base + 192);
		const float4 splits = asfloat(ShadowLightLatch.Load4(base + 256));
		light.EndSplitDistances = splits.xy;
		light.StartSplitDistances = splits.zw;
		return light;
	}
#else
	Texture2DArray<float4> TexDirectShadows : register(AP_DIRECT_SHADOW_REGISTER);
	StructuredBuffer<DirectionalShadowLightData> DirectionalShadowLights : register(t98);

	uint ShadowLightCount()
	{
		uint lightCount, lightStride;
		DirectionalShadowLights.GetDimensions(lightCount, lightStride);
		return lightCount;
	}
	DirectionalShadowLightData ShadowLight() { return DirectionalShadowLights[0]; }
#endif

	const static uint nStep = 30;
	const static float rcpNStep = rcp(nStep);

	float3 GetVolumetricCloudTransmittance(float3 posWorldRel)
	{
		return GetDirlightTransmittance(posWorldRel + FrameBuffer::CameraPosAdjust.xyz, SampTr);
	}

	struct ShadowRayData
	{
		float2 endSplitDistances;
		float2 viewOriginZW;
		float2 viewDirectionZW;
		float3 lightOrigin[2];
		float3 lightDirection[2];
		bool terrainReady;
	};

	ShadowRayData BuildShadowRay(float3 dir)
	{
		const uint lightCount = ShadowLightCount();
		uint shadowWidth, shadowHeight, shadowLayers;
		TexDirectShadows.GetDimensions(shadowWidth, shadowHeight, shadowLayers);
		DirectionalShadowLightData light = (DirectionalShadowLightData)0;
		if (lightCount > 0u && shadowWidth > 0u && shadowHeight > 0u && shadowLayers >= 2u)
			light = ShadowLight();
		ShadowRayData ray;
		uint terrainWidth, terrainHeight;
		TerrainShadows::ShadowHeightTexture.GetDimensions(terrainWidth, terrainHeight);
		ray.terrainReady = terrainWidth > 0u && terrainHeight > 0u;
		ray.endSplitDistances = light.EndSplitDistances;
		ray.viewOriginZW = mul(FrameBuffer::CameraViewProj, float4(0, 0, 0, 1)).zw;
		ray.viewDirectionZW = mul(FrameBuffer::CameraViewProj, float4(dir, 0)).zw;
		[unroll] for (uint cascade = 0; cascade < 2; ++cascade)
		{
			ray.lightOrigin[cascade] = mul(light.ShadowProj[cascade], float4(FrameBuffer::CameraPosAdjust.xyz, 1)).xyz;
			ray.lightDirection[cascade] = mul(light.ShadowProj[cascade], float4(dir, 0)).xyz;
		}
		return ray;
	}

	/** @brief 2x2 depth-compare of a cascade at a light-space position. */
	float GatherCascadeShadow(uint cascadeIndex, float3 positionLS)
	{
		float4 depths = TexDirectShadows.GatherRed(SampTr, float3(saturate(positionLS.xy), cascadeIndex), 0);
		return dot(float4(depths > positionLS.z), 0.25);
	}

	/** @brief Sun visibility at a camera-relative position from the cascade shadow map, fading to 1 at the cascade end. */
	float SampleCascadeShadow(float3 posWorldRel)
	{
		const uint lightCount = ShadowLightCount();
		uint shadowWidth, shadowHeight, shadowLayers;
		TexDirectShadows.GetDimensions(shadowWidth, shadowHeight, shadowLayers);
		// The work sits inside the tests. An early return behind an || of equality tests miscompiles on AMD RDNA2.
		float visibility = 1.0;
		[branch] if (min(lightCount, min(shadowWidth, shadowHeight)) > 0u && shadowLayers >= 2u)
		{
			const DirectionalShadowLightData light = ShadowLight();
			const float4 clipPos = mul(FrameBuffer::CameraViewProj, float4(posWorldRel, 1));
			const float shadowMapDepth = SharedData::GetScreenDepth(clipPos.z / clipPos.w);
			[branch] if (light.EndSplitDistances.y > 0.0 && shadowMapDepth < light.EndSplitDistances.y)
			{
				const uint cascadeIndex = shadowMapDepth >= light.EndSplitDistances.x ? 1u : 0u;
				const float3 positionLS = mul(light.ShadowProj[cascadeIndex], float4(posWorldRel + FrameBuffer::CameraPosAdjust.xyz, 1)).xyz;
				const float edgeFade = pow(saturate(shadowMapDepth / light.EndSplitDistances.y), 16);
				visibility = lerp(GatherCascadeShadow(cascadeIndex, positionLS), 1.0, edgeFade);
			}
		}
		return visibility;
	}

	float SampleShadow(float3 posWorldRel, float distance, ShadowRayData ray)
	{
		const SharedData::PhysSkyData data = SharedData::physSkyData;

		float shadow = 1.0;

		// Resolve scene and terrain occlusion first: a blocked sample needs neither
		// spherical cloud projection nor a 3D cloud-shadow lookup.
		{
			const float2 viewZW = ray.viewOriginZW + distance * ray.viewDirectionZW;
			float shadowMapDepth = SharedData::GetScreenDepth(viewZW.x / viewZW.y);

			[branch] if (ray.endSplitDistances.y > 0.0 &&
						 shadowMapDepth < ray.endSplitDistances.y)
			{
				uint cascadeIndex = shadowMapDepth >= ray.endSplitDistances.x ? 1u : 0u;

				float3 positionLS = ray.lightOrigin[cascadeIndex] + distance * ray.lightDirection[cascadeIndex];
				shadow *= GatherCascadeShadow(cascadeIndex, positionLS);
			}
		}
		[branch] if (all(shadow < 1e-8)) return 0;

		// terrain shadow
		float3 posWorldAbs = posWorldRel + FrameBuffer::CameraPosAdjust.xyz;
		if (ray.terrainReady)
			shadow *= TerrainShadows::GetTerrainShadow(posWorldAbs, SampTr);
		[branch] if (all(shadow < 1e-8)) return 0;

		float cloudShadow = 1.0;
		[branch] if (SharedData::cloudShadowsSettings.Opacity != 0.0)
			cloudShadow = CloudShadows::GetCloudShadowMult(posWorldRel, SampTr);
		shadow *= Remap(cloudShadow, data.cloudShadowRemapRange.x, data.cloudShadowRemapRange.y, 0, 1);
		[branch] if (all(shadow < 1e-8)) return 0;

		shadow *= Remap(dot(GetVolumetricCloudTransmittance(posWorldRel), float3(0.2126, 0.7152, 0.0722)), data.cloudShadowRemapRange.x, data.cloudShadowRemapRange.y, 0, 1);
		[branch] if (all(shadow < 1e-8)) return 0;

		return shadow;
	}

	float TraceShadow(float3 dir, float dist, float jitter)
	{
		const SharedData::PhysSkyData data = SharedData::physSkyData;

		if (dist <= 0.0)
			return 0.0;
		float distClamped = min(AP_MAX_DIST, dist);
		const ShadowRayData shadowRay = BuildShadowRay(dir);

		const float extGr = dot(data.rayleighScatter + data.aerosolAbsorption + data.aerosolScatter, 1 / 3.f);
		const float opticalDepth = extGr * distClamped;
		const float estContrib = 1 - exp(-opticalDepth);
		const float rcpExtGr = rcp(max(extGr, 1e-20));

		float shadow = 0;
		for (uint i = 1; i <= nStep; ++i) {
			const float sampleFraction = (i - 1 + jitter) * rcpNStep;
			float tSample;
			[branch] if (opticalDepth < 1e-3)
				tSample = sampleFraction * distClamped * (1.0 - 0.5 * (1.0 - sampleFraction) * opticalDepth);
			else tSample = -rcpExtGr * log(1 - sampleFraction * estContrib);

			float shadowSample = SampleShadow(dir * tSample, tSample, shadowRay);
			shadow += 1 - shadowSample;
		}

		return shadow * rcpNStep;
	}
}

#endif

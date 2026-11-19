#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif

// Dual-target (Common/OrgResources.hlsli). The ORG pass (ORGPhysicalSky, cs.physical-sky) gives every handle. The headers'
// own resources are EXTERN entries, and the ones this shader never reads are UNREAD. The directional shadow light is in the
// pass's latch (ApShadowLight, PhysicalSky/ApShadow.hlsli), with a clear flag after the light count.
#define ORG_SHARED_CONSTANTS
#define ORG_HEADER_RESOURCES
#include "Common/OrgResources.hlsli"

#define SHADOW_ACCUM_RESOURCES(X)                                  \
	X(SHARED, ConstantBuffer, Shared, 0)                           \
	X(SRV, Texture2D<float>, TexDepth, 0)                          \
	X(EXTERN, Texture2DArray<float4>, TexDirectShadows, 1)         \
	X(EXTERN_LATCH, DirectionalShadowLightData, ApShadowLight, 98) \
	X(EXTERN, Texture2D<float2>, ShadowHeightTexture, 3)           \
	X(EXTERN, TextureCube<float>, CloudShadowsTexture, 4)          \
	X(SRV, Texture3D<float>, TexShadowVolume, 5)                   \
	X(UAV, RWTexture2D<unorm float>, RWTexOutput, 0)               \
	X(SAMPLER, SamplerState, SampTr, 0)                            \
	X(UNREAD, TextureCube<float>, CloudSelfShadowTexture, 26)      \
	X(UNREAD, Texture2D<float4>, DepthTexture, 17)                 \
	X(UNREAD, Texture2D<float4>, MeanProfile, 69)                  \
	X(UNREAD, Texture2D<float4>, TexDiskPhaseLut, 106)
ORG_PUSH_BLOCK(SHADOW_ACCUM_RESOURCES(ORG_HANDLE))
SHADOW_ACCUM_RESOURCES(ORG_RESOURCE)

#define OMIT_PS_NAMESPACE
#define PS_PREPASS_RSRCS
#define PS_NO_RSRCS
#define PS_ENABLE_DIRLIGHT_TRANSMITTANCE
#define PS_LINEAR_SHADOW_SAMPLER
#define CLOUD_SHADOW_REGISTER t4
#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "PhysicalSky/Common.hlsli"

#if defined(HALF_RES)
#	define RES_MULT 2
#else
#	define RES_MULT 1
#endif

#include "PhysicalSky/ApShadow.hlsli"

// ORG only: the latch's word after the light count. Set, the pass writes no shadow (the map menu, as D3D11 clears it).
bool ClearsShadow()
{
#if defined(CS_ORG_BINDLESS)
	return AtmosphereShadow::ShadowLightLatch.Load(OrgOffset_ApShadowLight + AtmosphereShadow::kShadowLightBytes + 4) != 0u;
#else
	return false;
#endif
}

float TraceShadow(float2 pixelCenter, float depth, float jitter)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	const float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(pixelCenter * data.rcpTexDim);
	const bool skyRay = depth >= 1.0;
	float4 posWorld = float4(2 * float2(uv.x, -uv.y + 1) - 1, skyRay ? 0.5 : depth, 1);
	posWorld = mul(FrameBuffer::CameraViewProjInverse, posWorld);
	posWorld.xyz /= posWorld.w;
	const float distance = length(posWorld.xyz);
	return AtmosphereShadow::TraceShadow(posWorld.xyz / max(distance, 1e-20), skyRay ? AP_MAX_DIST : distance, jitter);
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	const uint2 frameDim = uint2(SharedData::physSkyData.frameDim);
	const uint2 pxCoords = tid * RES_MULT;
	if (any(pxCoords >= frameDim))
		return;

	[branch] if (ClearsShadow())
	{
		[unroll] for (uint j = 0; j < RES_MULT * RES_MULT; ++j)
		{
			const uint2 outputPixel = pxCoords + uint2(j % RES_MULT, j / RES_MULT);
			if (all(outputPixel < frameDim))
				RWTexOutput[outputPixel] = 0.0;
		}
		return;
	}

	const uint2 seed = Random::pcg2d(tid);
	const float jitter = Random::R2Modified(SharedData::FrameCountAlwaysActive, seed / 4294967295.f).x;

#if defined(HALF_RES)
	const uint2 lastCoords = min(pxCoords + 1u, frameDim - 1u);
	const float4 depths = float4(
		TexDepth[pxCoords],
		TexDepth[uint2(lastCoords.x, pxCoords.y)],
		TexDepth[uint2(pxCoords.x, lastCoords.y)],
		TexDepth[lastCoords]);
	const float4 linearDepths = SharedData::GetScreenDepths(depths);
	const float minDepth = min(min(linearDepths.x, linearDepths.y), min(linearDepths.z, linearDepths.w));
	const float maxDepth = max(max(linearDepths.x, linearDepths.y), max(linearDepths.z, linearDepths.w));
	const bool sharedRay = maxDepth - minDepth <= minDepth * 0.01;
	const uint rayCount = sharedRay ? 1u : 4u;
	[loop] for (uint i = 0; i < rayCount; ++i)
	{
		const uint2 pixel = pxCoords + uint2(i & 1u, i >> 1u);
		if (any(pixel >= frameDim))
			continue;
		const float2 pixelCenter = sharedRay ? (float2(pxCoords) + float2(lastCoords) + 1.0) * 0.5 : pixel + 0.5;
		const float depth = sharedRay ? dot(depths, 0.25) : depths[i];
		const float shadow = TraceShadow(pixelCenter, depth, jitter);
		[branch] if (sharedRay)
		{
			[unroll] for (uint j = 0; j < 4; ++j)
			{
				const uint2 outputPixel = pxCoords + uint2(j & 1u, j >> 1u);
				if (all(outputPixel < frameDim))
					RWTexOutput[outputPixel] = shadow;
			}
		}
		else
		{
			RWTexOutput[pixel] = shadow;
		}
	}
#else
	RWTexOutput[pxCoords] = TraceShadow(pxCoords + 0.5, TexDepth[pxCoords], jitter);
#endif
}

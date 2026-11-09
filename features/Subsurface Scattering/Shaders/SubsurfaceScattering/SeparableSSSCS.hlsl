// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below. The ORG passes
// (ORGSubsurfaceScattering.cpp) give heap handles in the push block, the shared constant
// buffers' too (ORG_SHARED_CONSTANTS).
#define ORG_SHARED_CONSTANTS
#include "Common/OrgResources.hlsli"

#define SEPARABLE_SSS_RESOURCES(X)              \
	X(SHARED, ConstantBuffer, Shared, 0)        \
	X(UAV, RWTexture2D<float4>, SSSRW, 0)       \
	X(SRV, Texture2D<float4>, ColorTexture, 0)  \
	X(SRV, Texture2D<float4>, DepthTexture, 1)  \
	X(SRV, Texture2D<float4>, MaskTexture, 2)   \
	X(SRV, Texture2D<float4>, AlbedoTexture, 3) \
	X(SRV, Texture2D<float4>, NormalTexture, 4) \
	X(SAMPLER, SamplerState, PointSampler, 0)   \
	X(LATCH, SSSConstants, PerFrameSSS, 1)
ORG_PUSH_BLOCK(SEPARABLE_SSS_RESOURCES(ORG_HANDLE))

#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "SubsurfaceScattering/SSSCommon.hlsli"
SEPARABLE_SSS_RESOURCES(ORG_RESOURCE)

#if defined(BURLEY)
#	include "SubsurfaceScattering/Burley.hlsli"
#else
#	include "SubsurfaceScattering/SeparableSSS.hlsli"
#endif

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	// Early exit if dispatch thread is outside screen bounds
	if (any(DTid.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	float2 texCoord = (DTid.xy + 0.5) * SharedData::BufferDim.zw;

#if defined(BURLEY)

	float sssAmount = MaskTexture[DTid.xy].x;

	if (sssAmount > 0.0) {
		bool humanProfile = MaskTexture[DTid.xy].y > 0.0;

		float4 color = BurleyNormalizedSS(DTid.xy, texCoord, sssAmount, humanProfile, SSSRW[DTid.xy]);
		SSSRW[DTid.xy] = max(0, color);
	}

#elif defined(HORIZONTAL)

	float sssAmount = MaskTexture[DTid.xy].x;
	bool humanProfile = MaskTexture[DTid.xy].y > 0.0;

	float4 color = SSSSBlurCS(texCoord, float2(1.0, 0.0), sssAmount, humanProfile);
	SSSRW[DTid.xy] = max(0, color);

#else

	float sssAmount = MaskTexture[DTid.xy].x;

	if (sssAmount > 0.0) {
		bool humanProfile = MaskTexture[DTid.xy].y > 0.0;

		float4 originalColor = SSSRW[DTid.xy];
		float4 color = SSSSBlurCS(texCoord, float2(0.0, 1.0), sssAmount, humanProfile);
		float3 albedo = SSSDecodeAlbedo(AlbedoTexture[DTid.xy].rgb);
		color.rgb = SSSApplyAlbedo(color.rgb, ColorManagement::SceneToLinear(originalColor.rgb), albedo, PerFrameSSS.ScatterMode);
		color.rgb = ColorManagement::LinearToScene(color.rgb);
		SSSRW[DTid.xy] = float4(color.rgb, originalColor.a);
	}

#endif
}

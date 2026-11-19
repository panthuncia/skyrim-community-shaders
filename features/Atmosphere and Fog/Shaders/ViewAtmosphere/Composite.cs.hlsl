// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below. The ORG pass
// (src/Features/ViewAtmosphere/ORGViewAtmosphere.cpp) passes heap handles in the push block, with the
// shared constants and the included feature headers' resources. The list is the same in every permutation.
#define COMPUTESHADER
#define PS_SKY_SAMPLERS
#define MEDIUM_COMPOSITE
#if defined(CS_ORG_BINDLESS)
#	define ORG_SHARED_CONSTANTS
#	define ORG_HEADER_RESOURCES
#	define VIEW_ATMOSPHERE_ORG_HANDLES
#endif
#include "Common/OrgResources.hlsli"

#define VIEW_ATMOSPHERE_COMPOSITE_RESOURCES(X)                                         \
	X(SHARED, ConstantBuffer, Shared, 0)                                               \
	X(SAMPLER, SamplerState, SampColorSampler, 0)                                      \
	X(SRV, Texture2D<float>, TexMediumDepth, 40)                                       \
	X(UAV, RWTexture2D<float4>, MainRW, 0)                                             \
	X(EXTERN, Texture3D<float4>, ViewAtmosphereLuminance, 125)                         \
	X(EXTERN, Texture3D<float4>, ViewAtmosphereTransmittance, 126)                     \
	X(EXTERN, StructuredBuffer<uint>, ViewAtmosphereData, 127)                         \
	X(EXTERN, Texture3D<float4>, ExponentialHeightFogIntegratedLightScattering, 19)    \
	X(EXTERN, Texture3D<float4>, ExponentialHeightFogIntegratedLightScatteringFar, 22) \
	X(EXTERN, Texture2D<sh2>, EnvIBLTexture, 76)                                       \
	X(EXTERN, Texture2D<sh2>, SkyIBLTexture, 77)                                       \
	X(EXTERN, TextureCube<float3>, EnvTexture, 31)                                     \
	X(UNREAD, TextureCube<float3>, EnvReflectionsTexture, 30)                          \
	X(UNREAD, Texture2D<float4>, DepthTexture, 17)
ORG_PUSH_BLOCK(VIEW_ATMOSPHERE_COMPOSITE_RESOURCES(ORG_HANDLE))
VIEW_ATMOSPHERE_COMPOSITE_RESOURCES(ORG_RESOURCE)

#include "Common/SharedData.hlsli"
#ifdef PHYSICAL_SKY
#	include "PhysicalSky/Common.hlsli"
#endif
#ifdef EXP_HEIGHT_FOG
#	include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#endif
#include "Common/ViewMedium.hlsli"

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint2 size;
	MainRW.GetDimensions(size.x, size.y);
	const uint2 activeSize = uint2(clamp(float2(size) * FrameBuffer::DynamicResolutionParams1.xy, 1.0, float2(size)));
	if (any(pixel >= activeSize))
		return;
	const float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition((float2(pixel) + 0.5) / float2(size));
	const float depth = TexMediumDepth[pixel];
	const bool fullCloudRay = depth >= 1.0 - 1e-6;
	float4 position = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2, -2) + float2(-1, 1), depth, 1));
	position.xyz /= position.w;
	float4 color = MainRW[pixel];
	color.rgb = PreExposure::Encode(ViewMedium::CompositeViewMedium(PreExposure::Decode(color.rgb), position.xyz, uv, SampColorSampler, fullCloudRay));
	MainRW[pixel] = color;
}

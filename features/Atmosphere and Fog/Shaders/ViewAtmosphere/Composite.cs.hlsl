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

#if defined(CS_ORG_BINDLESS) && defined(VIEW_ATMOSPHERE_COMPOSITE_DEBUG)
// Debug views of the ORG build (CS_ORG_VIEW_ATMOSPHERE_DEBUG). They write finite values only.
// 1: red where kMAIN held a non-finite value, green where the medium is non-finite, blue where the result is. Else the scene, dimmed.
// 2: the distance to the pixel, white at 10 km. Red where the depth reads as the sky.
// 3: the medium's transmittance. White is no fog. Magenta where it is non-finite.
// 4: the medium's luminance, times 4. Magenta where it is non-finite.
// 5: red where the shared medium is on, green where the near fog volume has a size, blue where the far one has. Dim while volumetric fog is off.
// 6: kMAIN written back unchanged.
float3 DebugView(float3 sceneColor, float3 position, float2 uv, bool fullCloudRay)
{
	if (VIEW_ATMOSPHERE_COMPOSITE_DEBUG == 6)
		return sceneColor;
	if (VIEW_ATMOSPHERE_COMPOSITE_DEBUG == 2)
		return fullCloudRay ? float3(1, 0, 0) : saturate(length(position) * GAME_UNIT_TO_M / 10000.0).xxx;
	if (VIEW_ATMOSPHERE_COMPOSITE_DEBUG == 5) {
		float3 flags = float3(ViewAtmosphere::Enabled() ? 1.0 : 0.0, 0.0, 0.0);
#	ifdef EXP_HEIGHT_FOG
		uint width, height, depth;
		ExponentialHeightFogIntegratedLightScattering.GetDimensions(width, height, depth);
		flags.y = width * height * depth != 0 ? 1.0 : 0.0;
		ExponentialHeightFogIntegratedLightScatteringFar.GetDimensions(width, height, depth);
		flags.z = width * height * depth != 0 ? 1.0 : 0.0;
		if (!ExponentialHeightFog::ShouldApplyVolumetricFog())
			flags *= 0.25;
#	endif
		return flags;
	}
	const ViewAtmosphere::Medium medium = ViewMedium::SampleViewMedium(position, uv, SampColorSampler, fullCloudRay);
	if (VIEW_ATMOSPHERE_COMPOSITE_DEBUG == 3)
		return all(isfinite(medium.transmittance)) ? medium.transmittance : float3(1, 0, 1);
	if (VIEW_ATMOSPHERE_COMPOSITE_DEBUG == 4)
		return all(isfinite(medium.luminance)) ? medium.luminance * 4.0 : float3(1, 0, 1);
	const float3 result = ViewMedium::CompositeViewMedium(PreExposure::Decode(sceneColor), position, uv, SampColorSampler, fullCloudRay);
	const bool badInput = !all(isfinite(sceneColor));
	const bool badMedium = !all(isfinite(medium.luminance)) || !all(isfinite(medium.transmittance));
	const bool badResult = !all(isfinite(result));
	if (badInput || badMedium || badResult)
		return float3(badInput ? 1.0 : 0.0, badMedium ? 1.0 : 0.0, badResult ? 1.0 : 0.0);
	return sceneColor * 0.25;
}
#endif

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
#if defined(CS_ORG_BINDLESS) && defined(VIEW_ATMOSPHERE_COMPOSITE_DEBUG)
	MainRW[pixel] = float4(DebugView(color.rgb, position.xyz, uv, fullCloudRay), color.a);
	return;
#endif
	color.rgb = PreExposure::Encode(ViewMedium::CompositeViewMedium(PreExposure::Decode(color.rgb), position.xyz, uv, SampColorSampler, fullCloudRay));
	MainRW[pixel] = color;
}

// Aerial perspective and the cloud composite on opaque pixels, as the D3D11 deferred composite applies them under
// PHYSICAL_SKY (DeferredCompositeCS.hlsl). The render graph's composite is built without PHYSICAL_SKY, because its push
// block has no room for these inputs. So this pass runs right after it (ORGPhysicalSky, cs.physical-sky.composite),
// reads kMAIN, applies the same code and writes it back.
// Dual-target in form only: D3D11 keeps the composite's own path. The ORG pass gives every handle. ViewAtmosphere's
// medium stays out (no VIEW_ATMOSPHERE_ORG_HANDLES), so ViewAtmosphere::Enabled() is false here.
#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif

#define ORG_SHARED_CONSTANTS
#define ORG_HEADER_RESOURCES
#include "Common/OrgResources.hlsli"

#define APPLY_AP_RESOURCES(X)                                   \
	X(SHARED, ConstantBuffer, Shared, 0)                        \
	X(UAV, RWTexture2D<float4>, MainRW, 0)                      \
	X(SRV, Texture2D<float>, SceneDepth, 0)                     \
	X(EXTERN, Texture3D<float4>, TexApLut, 63)                  \
	X(EXTERN, Texture2D<unorm float>, TexApShadow, 64)          \
	X(EXTERN, Texture2D<float>, TexVolTr, 110)                  \
	X(EXTERN, Texture2D<float4>, TexVolLum, 111)                \
	X(EXTERN, Texture2D<float4>, TexLightningChannels, 113)     \
	X(EXTERN, SamplerState, SampSv, 4)                          \
	X(UNREAD, SamplerState, SampTr, 3)                          \
	X(UNREAD, Texture2D<float4>, TexTrLut, 61)                  \
	X(UNREAD, Texture2D<float4>, TexSvLut, 62)                  \
	X(UNREAD, Texture2D<float4>, TexSkyTail, 66)                \
	X(UNREAD, TextureCube<float4>, TexVolCubeDepth, 67)         \
	X(UNREAD, Texture2D<float4>, MeanProfile, 69)               \
	X(UNREAD, Texture2D<float4>, TexDiskPhaseLut, 106)          \
	X(UNREAD, TextureCube<float4>, TexLightningReflection, 109) \
	X(UNREAD, Texture3D<float>, TexShadowVolume, 112)           \
	X(UNREAD, TextureCube<float>, TexVolCubeTr, 114)            \
	X(UNREAD, TextureCube<float4>, TexVolCubeLum, 115)          \
	X(UNREAD, Texture2D<float4>, DepthTexture, 17)              \
	X(UNREAD, TextureCube<float>, CloudShadowsTexture, 25)      \
	X(UNREAD, TextureCube<float>, CloudSelfShadowTexture, 26)
ORG_PUSH_BLOCK(APPLY_AP_RESOURCES(ORG_HANDLE))
APPLY_AP_RESOURCES(ORG_RESOURCE)

#include "Common/FrameBuffer.hlsli"
#include "Common/PreExposure.hlsli"
#include "Common/SharedData.hlsli"

// The pixel-stage header path with the sky's samplers: SampSv is the one the composite passes.
#define PS_SKY_SAMPLERS
#undef IBL
#include "PhysicalSky/Common.hlsli"

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	// As DeferredCompositeCS.hlsl. The depth is zero where the D3D11 composite binds none (no Dynamic Cubemaps).
	float2 uv = float2(dispatchID.xy + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;
	const float depth = SceneDepth[dispatchID.xy];

	// The work sits inside the test, with no early return behind it (AMD RDNA2).
	const bool apply = SharedData::physSkyData.enabled != 0u && depth < 1 - 1e-6;
	[branch] if (apply)
	{
		float4 positionWS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
		positionWS = mul(FrameBuffer::CameraViewProjInverse, positionWS);
		positionWS.xyz = positionWS.xyz / positionWS.w;

		float3 color = PreExposure::Decode(MainRW[dispatchID.xy].xyz);
		color = PhysSky::CompositeAerialPerspective(color, normalize(positionWS.xyz), dispatchID.xy, length(positionWS.xyz), PhysSky::SampSv);
		MainRW[dispatchID.xy] = float4(PreExposure::Encode(color), 1.0);
	}
}

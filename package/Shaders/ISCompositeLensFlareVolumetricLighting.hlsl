#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0-t1 and s0-s1 from the heap, FrameBuffer
// and SharedData from the shared constants, PerGeometry from the engine-constants capture through the pass's latch.
#	define ORG_SHARED_CONSTANTS
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISCOMPOSITE_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, VLSourceTex, 0) X(SRV, Texture2D<float4>, LFSourceTex, 1) X(SAMPLER, SamplerState, VLSourceSampler, 0) X(SAMPLER, SamplerState, LFSourceSampler, 1) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISCOMPOSITE_ORG_RESOURCES(ORG_HANDLE))
ISCOMPOSITE_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/Color.hlsli"
#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float3 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
struct ISCompositePerGeometry
{
	float4 VolumetricLightingColor;
};
ORG_ENGINE_CONSTANTS(ISCompositePerGeometry, PerGeometry, PerGeometryAt)
static float4 VolumetricLightingColor = PerGeometry.VolumetricLightingColor;
#	else
SamplerState VLSourceSampler : register(s0);
SamplerState LFSourceSampler : register(s1);

Texture2D<float4> VLSourceTex : register(t0);
Texture2D<float4> LFSourceTex : register(t1);

cbuffer PerGeometry : register(b2)
{
	float4 VolumetricLightingColor : packoffset(c0);
};
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float3 color = 0.0.xxx;

#	if defined(VOLUMETRIC_LIGHTING)
	float2 screenPosition = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);
	float volumetricLightingPower = VLSourceTex.Sample(VLSourceSampler, screenPosition).x;
	float3 volumetricLightingColor = VolumetricLightingColor.xyz;
#		if defined(EFFECTS11)
	if (SharedData::enbSettings.Enable) {
		// Preset intensities are far above vanilla, so the accumulated power must be capped or the rays become a flat veil
		volumetricLightingPower = saturate(volumetricLightingPower);
		// The game blends the custom colour with the sunlight, which Effects 11 has graded as direct lighting; undo that grade on the sunlight share
		float3 customColorShare = SharedData::enbSettings.GameVolumetricRaysCustomColor * SharedData::enbSettings.GameVolumetricRaysCustomContribution;
		volumetricLightingColor = (volumetricLightingColor - customColorShare) * SharedData::enbSettings.GameVolumetricRaysLightScale + customColorShare;
		volumetricLightingColor = lerp(volumetricLightingColor, dot(volumetricLightingColor, 1.0 / 3.0), SharedData::enbSettings.VolumetricRaysDesaturation);
		volumetricLightingColor *= SharedData::enbSettings.VolumetricRaysColorFilter;
	}
#		endif
	color += volumetricLightingColor * volumetricLightingPower;
#	endif

#	if defined(LENS_FLARE)
	float3 lensFlareColor = LFSourceTex.Sample(LFSourceSampler, input.TexCoord).xyz;
#		if defined(ENABLE_LL)
	color += TransferFunctions::GameGammaToLinear(lensFlareColor);
#		else
	color += lensFlareColor;
#		endif
#	endif

	psout.Color = color;

	return psout;
}
#endif

#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0 and s0 from the heap, the shared
// constants, and PerGeometry from the engine-constants capture through the pass's latch.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISRADIALBLUR_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, ImageTex, 0) X(SAMPLER, SamplerState, ImageSampler, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISRADIALBLUR_ORG_RESOURCES(ORG_HANDLE))
ISRADIALBLUR_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
static float4 Params = OrgEngineRow(PerGeometryAt, 0);
static float4 Center = OrgEngineRow(PerGeometryAt, 1);
#	else
SamplerState ImageSampler : register(s0);

Texture2D<float4> ImageTex : register(t0);

cbuffer PerGeometry : register(b2)
{
	float4 Params : packoffset(c0);
	float4 Center : packoffset(c1);
};
#	endif

float2 GetSourceTexCoord(float2 texCoord)
{
	return Center.w > 0.5 ? FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(texCoord) : texCoord;
}

float GetCircleParam(float centerDistance, float param1, float param2)
{
	float circleDistance = max(0, centerDistance - param1);
	float result = 0;
	if (circleDistance > 0) {
		result = Params.x * (1 - 1 / (param2 * circleDistance + 1));
	}
	return result;
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 offset = Center.xy - input.TexCoord;
	float centerDistance = length(offset);
	float2 sampleDelta =
		(1. / NUM_STEPS) * (normalize(offset) * max(0, GetCircleParam(centerDistance, Params.z, Params.y) -
														   GetCircleParam(centerDistance, Center.z, Params.w)));

	float4 color = 0;
	for (float sampleIndex = -NUM_STEPS; sampleIndex <= NUM_STEPS; ++sampleIndex) {
		float2 texCoord = input.TexCoord + sampleDelta * sampleIndex;
		float2 adjustedTexCoord = GetSourceTexCoord(texCoord);
		float4 currentColor = ImageTex.SampleLevel(ImageSampler, adjustedTexCoord, 0);
		color += currentColor;
	}
	color *= (1. / (2. * NUM_STEPS + 1.));

	psout.Color = color;

	return psout;
}
#endif

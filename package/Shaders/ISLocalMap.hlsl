#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0 and s0 from the heap, the shared constants, and
// PerGeometry from the engine-constants capture through the pass's latch. The vertex stage is the graph's fullscreen triangle.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISLOCALMAP_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, ImageTex, 0) X(SAMPLER, SamplerState, ImageSampler, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISLOCALMAP_ORG_RESOURCES(ORG_HANDLE))
ISLOCALMAP_ORG_RESOURCES(ORG_RESOURCE)
#endif

#include "Common/Color.hlsli"
#include "Common/DummyVSTexCoord.hlsl"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS)
static float4 TexelSize = OrgEngineRow(PerGeometryAt, 0);
#	else
SamplerState ImageSampler : register(s0);

Texture2D<float4> ImageTex : register(t0);

cbuffer PerGeometry : register(b2)
{
	float4 TexelSize : packoffset(c0);
};
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float3 weight = float3(0.5, 0.25, 1);
	float3 colorLR = 0;
	float3 colorBT = 0;
	[unroll] for (int j = -1; j <= 1; ++j)
	{
		[unroll] for (int i = -1; i <= 1; ++i)
		{
			if (i == 0 && j == 0) {
				continue;
			}

			float3 currentColor =
				ImageTex.Sample(ImageSampler, input.TexCoord + float2(i * TexelSize.x, j * TexelSize.y))
					.xyz;

			float centerMul = 1;
			if (i == 0 || j == 0) {
				centerMul = 2;
			}

			colorLR += -i * (centerMul * weight) * currentColor;
			colorBT += -j * (centerMul * weight) * currentColor;
		}
	}

	float4 colorCC = ImageTex.Sample(ImageSampler, input.TexCoord);
	float luminance = Color::RGBToLuminanceAlternative(colorCC.xyz);

	float alpha = (dot(4 * (pow(colorLR, 2) + pow(colorBT, 2)), 1.75) + luminance) * (1 - colorCC.w);
	float2 edgeFadeFactor = 1 - pow(2 * abs(input.TexCoord - 0.5), 5);

	psout.Color.xyz = 1.04 * luminance;
	psout.Color.w = alpha * edgeFadeFactor.x * edgeFadeFactor.y;

	return psout;
}
#endif

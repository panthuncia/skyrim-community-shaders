#if defined(CS_ORG_BINDLESS)
// The ORG build of the pixel stage (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): only the shared constants, and
// PerGeometry from the engine-constants capture through the pass's latch. The vertex stage is the graph's fullscreen triangle.
#	define ORG_SHARED_CONSTANTS
// FrameBuffer is the PerFrame the engine's draw had bound (captured with PerGeometry), not CS's shared snapshot.
#	define ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISSIMPLECOLOR_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(ISSIMPLECOLOR_ORG_RESOURCES(ORG_HANDLE))
ISSIMPLECOLOR_ORG_RESOURCES(ORG_RESOURCE)
#endif

#if defined(DISPLAY_DEPTH)
#	include "Common/DummyVSTexCoord.hlsl"
#else
#	include "Common/DummyVS.hlsl"
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
#	if defined(CS_ORG_BINDLESS) && !defined(DISPLAY_DEPTH)
// The plain colour fill (ISSimpleColor); the depth display (ISDisplayDepth, a debug view) has no ORG build.
static float4 Color = OrgEngineRow(PerGeometryAt, 0);
#	else
SamplerState DepthSampler : register(s0);
SamplerState uintStencilSampler : register(s1);

Texture2D<float4> DepthTex : register(t0);
Texture2D<uint4> uintStencilTex : register(t1);

cbuffer PerGeometry : register(b2)
{
	float4 Color : packoffset(c0);
};
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#	if defined(DISPLAY_DEPTH)
	float depth;
	if (1e-5 < Color.z) {
		uint2 dimensions;
		uint numberOfLevels;
		uintStencilTex.GetDimensions(0, dimensions.x, dimensions.y, numberOfLevels);
		float3 location = float3(input.TexCoord * dimensions, 0);
		depth = uintStencilTex.Load(location).x;
	} else {
		depth = DepthTex.SampleLevel(DepthSampler, input.TexCoord, 0).x;
	}
	float screenDepth = saturate((-Color.x + depth) / (Color.y - Color.x));
	psout.Color.xyz = (screenDepth * -2 + 3) * (screenDepth * screenDepth);
	psout.Color.w = 1;
#	else
	psout.Color = Color;
#	endif

	return psout;
}
#endif

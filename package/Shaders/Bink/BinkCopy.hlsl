// Bink's tile copy: one textured quad per 256x256 tile of a decoded video frame (docs/development/p5/bink.md).
//
// The engine's own pair (SE 1.5.97 VS 0x141e0bcd0, PS 0x141e0bf10; AE 1.6.1170 0x1420300b0 and 0x1420302f0, the same bytes)
// has no HLSL in the executable. This is that pair written out. The FXC build of each stage gives the same instructions as the
// engine's DXBC:
// - VS: position.xyz with w = 1, TEXCOORD passed through.
// - PS: TexBase sampled at TEXCOORD, with red and blue swapped. The CPU decoder writes each pixel in BGRA order into an
//   R8G8B8A8_UNORM texture, and this swap puts the channels back.
//
// The ORG build (CS_ORG_BINDLESS, src/RenderGraph/Engine/Bink/BinkGraph.cpp) has no vertex buffer. The vertex stage makes the
// engine's quad from the tile's rectangle in the push block. Its corners are top left, bottom left, bottom right, top right, as
// BSMovie::CreateTileQuad (SE 0x1410a2720) builds them, drawn as triangles 0-1-2 and 0-2-3.

#include "Common/OrgResources.hlsli"

struct BinkQuad
{
	float4 Rect;  // the tile's corners in clip space: left, top, right, bottom
};

#define BINK_COPY_RESOURCES(X)            \
	X(SRV, Texture2D<float4>, TexBase, 0) \
	X(SAMPLER, SamplerState, SampBase, 0) \
	X(PUSH, BinkQuad, Quad, 0)
ORG_PUSH_BLOCK(BINK_COPY_RESOURCES(ORG_HANDLE))
BINK_COPY_RESOURCES(ORG_RESOURCE)

struct VS_OUTPUT
{
	float4 Position: SV_POSITION;
	float2 TexCoord: TEXCOORD0;
};

#if defined(CS_ORG_BINDLESS)
VS_OUTPUT VSMain(uint vertexID : SV_VertexID)
{
	// Corner per vertex: the engine's index list 0, 1, 2, 0, 2, 3.
	const uint corner = vertexID < 3 ? vertexID : (vertexID == 3 ? 0 : vertexID - 2);
	const float2 uv = float2(corner >= 2 ? 1.0 : 0.0, (corner == 1 || corner == 2) ? 1.0 : 0.0);

	VS_OUTPUT output;
	output.Position = float4(lerp(Quad.Rect.xy, Quad.Rect.zw, uv), 0.0, 1.0);
	output.TexCoord = uv;
	return output;
}
#else
struct VS_INPUT
{
	float4 Position: POSITION0;
	float2 TexCoord: TEXCOORD0;
};

VS_OUTPUT VSMain(VS_INPUT input)
{
	VS_OUTPUT output;
	output.Position = float4(input.Position.xyz, 1.0);
	output.TexCoord = input.TexCoord;
	return output;
}
#endif

float4 PSMain(VS_OUTPUT input) : SV_Target0
{
	return TexBase.Sample(SampBase, input.TexCoord).zyxw;
}

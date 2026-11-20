// The fullscreen vertex stage of ImageSpace's render-size steps (src/RenderGraph/Engine/ImageSpace, Spec::latchViewport).
// The pass records the whole output as its render area. This stage draws a quad, two triangles, in the part of it the draw's
// latch names, so a new dynamic-resolution ratio is a new latch value and never a new recording (frame revision plan, step 8).
// It covers the same pixels as a viewport of that size, and TEXCOORD0 runs from (0,0) to (1,1) across it, as the
// fullscreen triangle's does across a viewport (FullscreenVS.hlsl).
//
// The latch is the draw's OrgEngineConstantsAt (Common/OrgEngineConstants.hlsli); its Viewport holds the scale and offset
// as fractions of the render area. ORG_VIEWPORT_LATCH_WORD is the push word of the latch's handle; the byte offset follows it.
// The C++ side sets it from the pixel stage's resource list (ImageSpace.cpp, DescribeRaster).

#if !defined(ORG_VIEWPORT_LATCH_WORD)
#	error ORG_VIEWPORT_LATCH_WORD names the push word of the draw's constants latch
#endif

#define ORG_VIEWPORT_LATCH_ROWS (ORG_VIEWPORT_LATCH_WORD / 4)
#define ORG_VIEWPORT_LATCH_REST (ORG_VIEWPORT_LATCH_WORD % 4)

// The push block up to the latch's two words. The pixel stage declares the rest.
cbuffer OrgBindings : register(b0)
{
#if ORG_VIEWPORT_LATCH_ROWS > 0
	uint4 OrgViewportPadRows[ORG_VIEWPORT_LATCH_ROWS];
#endif
#if ORG_VIEWPORT_LATCH_REST > 0
	uint OrgViewportPad0;
#endif
#if ORG_VIEWPORT_LATCH_REST > 1
	uint OrgViewportPad1;
#endif
#if ORG_VIEWPORT_LATCH_REST > 2
	uint OrgViewportPad2;
#endif
	uint OrgViewportLatchHandle;
	uint OrgViewportLatchOffset;
};

// OrgEngineConstantsAt's Viewport member: eight uints before it.
static const uint kOrgViewportByteOffset = 32;

struct FullscreenVSOutput
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
};

FullscreenVSOutput main(uint vertexID : SV_VertexID)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[OrgViewportLatchHandle];
	const float4 viewport = asfloat(latch.Load4(OrgViewportLatchOffset + kOrgViewportByteOffset));
	// Two triangles: (0,0) (1,0) (0,1), then (0,1) (1,0) (1,1).
	const uint corner = vertexID < 3 ? vertexID : vertexID - 2;
	const float2 uv = float2(corner & 1, corner >> 1);
	FullscreenVSOutput output;
	output.TexCoord = uv;
	const float2 area = viewport.zw + uv * viewport.xy;
	output.Position = float4(area * 2.0f - 1.0f, 0.0f, 1.0f);
	output.Position.y = -output.Position.y;
	return output;
}

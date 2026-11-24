// BSLensFlare: one sprite of a lens flare, transcribed from the vanilla DXBC. The engine has no source file name for this
// shader (its originalShaderName is null), so ShaderCache maps it to this file. The constant names are the engine's
// (the BSLensFlare constructor) and it fills them by name; the registers are vanilla's.

#if defined(CS_ORG_BINDLESS)
// The ORG build (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE). One pass draws every sprite of the frame, a quad each. Each
// sprite has a record in the pass's latch block, past the pass's own region (ChainSteps.inl, StageLensFlareSprite): the
// PerGeometry the engine captured for each stage, the heap index of its texture (t0), and its geometry's buffer view refs.
// The vertex stage pulls the sprite's vertices and hands the sprite's number to the pixel stage. t1 and s0 come from the heap.
#	define ORG_SHARED_CONSTANTS
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"

// The sprite pass's own latch region (LensFlareSpritesAt).
struct LensFlareSpritesAt
{
	uint RecordOffset;  // bytes from this region to the first sprite's record
	uint RecordStride;  // bytes between records
	uint Sprites;
	uint VerticesPerSprite;
	float4 Viewport;  // the engine's viewport in kMAIN as fractions: scale xy, then offset xy
};

// One sprite's record (LensFlareSpriteRecord).
struct LensFlareSpriteRecord
{
	uint4 PixelConstants;   // the pixel stage's PerGeometry: ring SRV, offset, bound size; then t0's heap index
	uint4 VertexConstants;  // the vertex stage's PerGeometry: ring SRV, offset, bound size; then the first index
	uint4 Buffers;          // the vertex buffer's ref, then the 16-bit index buffer's
	uint4 Layout;           // the engine's vertex layout (two words), then the vertex stride
};

// t0 is latched, so it has no handle here. The pass binds the latched slots' region anyway; each record holds the index instead.
#	define ISLENSFLARE_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, VisTex, 1) X(SAMPLER, SamplerState, LensFlareSampler, 0) X(LATCH, LensFlareSpritesAt, SpritesAt, 2) X(LATCH, OrgEngineSourcesAt, SourcesAt, 3)
ORG_PUSH_BLOCK(ISLENSFLARE_ORG_RESOURCES(ORG_HANDLE))
ISLENSFLARE_ORG_RESOURCES(ORG_RESOURCE)

LensFlareSpriteRecord LensFlareRecord(uint a_sprite)
{
	return OrgLatch_SpritesAt.Load<LensFlareSpriteRecord>(OrgOffset_SpritesAt + SpritesAt.RecordOffset + a_sprite * SpritesAt.RecordStride);
}

// One 16-byte row of a sprite's captured PerGeometry, 0 past its bound size. Sprites in one wave may have different ring chunks.
float4 LensFlareRow(uint4 a_constants, uint a_row)
{
	ByteAddressBuffer ring = ResourceDescriptorHeap[NonUniformResourceIndex(a_constants.x)];
	return a_row * 16 < a_constants.z ? asfloat(ring.Load4(a_constants.y + a_row * 16)) : 0.0.xxxx;
}
#endif

struct VS_INPUT
{
	float4 Position: POSITION0;
	float2 TexCoord: TEXCOORD0;
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
	float4 TexCoord: TEXCOORD0;  // xy: sprite UV, z: a tenth of the sprite's distance from the screen center
	float3 LightPos: TEXCOORD1;  // the light's screen position, passed through
#if defined(CS_ORG_BINDLESS)
	nointerpolation uint Sprite: TEXCOORD2;  // the sprite's record
#endif
};

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(VSHADER) && defined(CS_ORG_BINDLESS)
#	define DCLF_PULLED_VERTICES
#	include "Common/DCLFVertexPull.hlsli"

VS_OUTPUT main(uint a_vertexID: SV_VertexID)
{
	const uint sprite = a_vertexID / SpritesAt.VerticesPerSprite;
	const LensFlareSpriteRecord record = LensFlareRecord(sprite);

	// The vertex as the input assembler reads it: its 16-bit index, then its attributes.
	const uint indexNumber = record.VertexConstants.w + a_vertexID % SpritesAt.VerticesPerSprite;
	const DeviceRef indices = DeviceRef(record.Buffers.z, record.Buffers.w);
	const uint indexWord = DeviceLoad(uint, DeviceRefAdd(indices, (indexNumber * 2) & ~3u));
	const uint index = (indexNumber & 1) ? (indexWord >> 16) : (indexWord & 0xFFFF);
	DCLFDrawVertexLayout = record.Layout.xy;
	DCLFStream first = { DeviceRef(record.Buffers.x, record.Buffers.y), record.Layout.z };
	DCLFStream second = { DeviceRef(0, 0), 0u };
	DCLFStreamsAt(index, first, second);
	VS_INPUT input;
	input.Position = DCLFPullPosition(first, second);
	input.TexCoord = DCLFPullHalf2(1, first, second);

	const float4 UV = LensFlareRow(record.VertexConstants, 1);
	const float4 ScreenSpaceLightPos_PositionScale = LensFlareRow(record.VertexConstants, 2);

	VS_OUTPUT vsout;

	float2 position = ScreenSpaceLightPos_PositionScale.xy * 2.0 - 1.0;
	position = position * ScreenSpaceLightPos_PositionScale.w + input.Position.x * UV.xy;
	position = input.Position.y * UV.zw + position;

	vsout.TexCoord = float4(input.TexCoord.xy, length(position) * 0.1, 0);
	vsout.LightPos = ScreenSpaceLightPos_PositionScale.xyz;

	// The pass draws into all of kMAIN. Move the sprite into the engine's viewport there.
	const float2 screen = float2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5) * SpritesAt.Viewport.xy + SpritesAt.Viewport.zw;
	vsout.Position = float4(screen.x * 2.0 - 1.0, 1.0 - screen.y * 2.0, 0, 1);
	vsout.Sprite = sprite;

	return vsout;
}
#elif defined(VSHADER)
cbuffer PerGeometry : register(b2)
{
	float4 UV : packoffset(c1);                                 // xy and zw: the sprite's axes for the vertex x and y
	float4 ScreenSpaceLightPos_PositionScale : packoffset(c2);  // xy: light position in UV, w: how far along the light axis
};

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float2 position = ScreenSpaceLightPos_PositionScale.xy * 2.0 - 1.0;
	position = position * ScreenSpaceLightPos_PositionScale.w + input.Position.x * UV.xy;
	position = input.Position.y * UV.zw + position;

	vsout.Position = float4(position, 0, 1);
	vsout.TexCoord = float4(input.TexCoord.xy, length(position) * 0.1, 0);
	vsout.LightPos = ScreenSpaceLightPos_PositionScale.xyz;

	return vsout;
}
#endif

#if defined(PSHADER) && defined(CS_ORG_BINDLESS)
PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	const LensFlareSpriteRecord record = LensFlareRecord(input.Sprite);
	const float4 Color = LensFlareRow(record.PixelConstants, 0);
	const float4 VisScale = LensFlareRow(record.PixelConstants, 1);
	const float4 row2 = LensFlareRow(record.PixelConstants, 2);
	const float LightIndex = row2.x;
	const float GlobalIntensity = row2.y;
	Texture2D<float4> LensFlareTex = ResourceDescriptorHeap[NonUniformResourceIndex(record.PixelConstants.w)];

	const float visibility = dot(VisScale, VisTex.Load(int3((int)LightIndex, 0, 0)));
	const float3 tint = (visibility * Color.xyz) * GlobalIntensity;

	float2 texCoord = input.TexCoord.xy;
	if (Color.w > 0) {
		texCoord = (texCoord - 0.5) / (visibility + 0.001) + 0.5;
	}

	psout.Color = float4(tint * LensFlareTex.Sample(LensFlareSampler, texCoord).xyz, 0);

	return psout;
}
#elif defined(PSHADER)
SamplerState LensFlareSampler : register(s0);

Texture2D<float4> LensFlareTex : register(t0);
Texture2D<float4> VisTex : register(t1);  // one texel per light: the visibility the occlusion queries measured

cbuffer PerGeometry : register(b2)
{
	float4 Color : packoffset(c0);        // xyz: tint, w > 0: scale the sprite by its visibility
	float4 VisScale : packoffset(c1);     // weights of the visibility texel's channels
	float LightIndex : packoffset(c2.x);  // which visibility texel
	float GlobalIntensity : packoffset(c2.y);
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	const float visibility = dot(VisScale, VisTex.Load(int3((int)LightIndex, 0, 0)));
	const float3 tint = (visibility * Color.xyz) * GlobalIntensity;

	float2 texCoord = input.TexCoord.xy;
	if (Color.w > 0) {
		texCoord = (texCoord - 0.5) / (visibility + 0.001) + 0.5;
	}

	psout.Color = float4(tint * LensFlareTex.Sample(LensFlareSampler, texCoord).xyz, 0);

	return psout;
}
#endif

// The frame anchors' self-test (CS_ORG_ANCHOR_SELFTEST=1, src/RenderGraph/FrameAnchorSelfTest.cpp). At each anchor a raster pass
// writes the anchor's value into a small texture (PSMain), then a compute pass (CSMain) reads it back into the anchor's word of a
// buffer, adding one, and clears the texel. A pass run out of order reads the cleared texel. The compute pass also reads one texel of
// the engine's kMAIN, which proves the engine target import. Resources come from the descriptor heap by index (BasicRHI's ABI); the
// push block must match SelfTestConstants in FrameAnchorSelfTest.cpp.

cbuffer SelfTestConstants : register(b0)
{
	uint Value;         // what the raster pass writes
	uint TextureIndex;  // RWTexture2D<uint>: the anchor's texture
	uint OutputIndex;   // RWStructuredBuffer<uint>: word Anchor is the check, word 8 + Anchor kMAIN's texel
	uint Anchor;
	uint MainIndex;     // Texture2D<float4>: kMAIN, or 0xFFFFFFFF without it
	uint3 Padding;
}

static const uint2 kTexel = uint2(3, 5);

struct PSInput
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
};

uint PSMain(PSInput input) : SV_Target0
{
	return Value;
}

[numthreads(1, 1, 1)] void CSMain() {
	RWTexture2D<uint> texture = ResourceDescriptorHeap[TextureIndex];
	RWStructuredBuffer<uint> output = ResourceDescriptorHeap[OutputIndex];
	output[Anchor] = texture[kTexel] + 1;
	texture[kTexel] = 0;
	if (MainIndex != 0xFFFFFFFFu) {
		Texture2D<float4> main = ResourceDescriptorHeap[MainIndex];
		output[8 + Anchor] = asuint(main.Load(int3(0, 0, 0)).r);
	}
}

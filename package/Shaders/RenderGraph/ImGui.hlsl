// Dear ImGui's draws on the render graph (src/RenderGraph/ImGuiBasicRHI.cpp), the same as imgui_impl_dx11's shaders. There are no
// vertex buffers: the vertex stage reads ImGui's indices and vertices from one staging buffer by index (a ByteAddressBuffer from the
// descriptor heap), at the draw's offsets in the push block. Resources come from the descriptor heaps by index (BasicRHI's ABI); the
// push block must match DrawConstants in ImGuiBasicRHI.cpp.

cbuffer DrawConstants : register(b0)
{
	uint GeometryIndex;  // ByteAddressBuffer: this frame's vertices and indices
	uint VertexOffset;   // bytes: the draw's first vertex (ImDrawCmd::VtxOffset and its list's)
	uint IndexOffset;    // bytes: the draw's first index
	uint IndexSize;      // bytes per index: sizeof(ImDrawIdx), 2 or 4
	uint TextureIndex;   // Texture2D<float4>
	uint SamplerIndex;   // SamplerState: linear, clamped, the top mip only
	float2 Scale;        // ImGui's display rectangle to clip space
	float2 Translate;
	uint2 Padding;
}

// ImDrawVert: float2 pos, float2 uv, uint col (RGBA8, red in the low byte).
static const uint kVertexStride = 20;

struct VSOutput
{
	float4 Position: SV_POSITION;
	float4 Colour: COLOR0;
	float2 TexCoord: TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
	ByteAddressBuffer geometry = ResourceDescriptorHeap[GeometryIndex];
	const uint indexAddress = IndexOffset + vertexID * IndexSize;
	const uint word = geometry.Load(indexAddress & ~3u);
	const uint index = IndexSize == 4 ? word : ((indexAddress & 2u) ? word >> 16 : word & 0xFFFFu);
	const uint vertexAddress = VertexOffset + index * kVertexStride;
	const float2 position = asfloat(geometry.Load2(vertexAddress));
	const uint colour = geometry.Load(vertexAddress + 16);

	VSOutput output;
	output.Position = float4(position * Scale + Translate, 0.0f, 1.0f);
	output.TexCoord = asfloat(geometry.Load2(vertexAddress + 8));
	output.Colour = float4(colour & 0xFFu, (colour >> 8) & 0xFFu, (colour >> 16) & 0xFFu, colour >> 24) / 255.0f;
	return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
	Texture2D<float4> texture = ResourceDescriptorHeap[TextureIndex];
	SamplerState linearClamp = SamplerDescriptorHeap[SamplerIndex];
	return input.Colour * texture.Sample(linearClamp, input.TexCoord);
}

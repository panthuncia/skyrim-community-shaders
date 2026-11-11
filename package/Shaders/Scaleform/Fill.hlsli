#ifndef __SCALEFORM_FILL_DEPENDENCY_HLSL__
#define __SCALEFORM_FILL_DEPENDENCY_HLSL__

// The ORG side of Scaleform's fill shaders (FillVS.hlsl, FillPS.hlsl): what the render graph's Scaleform renderer
// (src/RenderGraph/Engine/Scaleform/) gives a draw. Only the ORG build (CS_ORG_BINDLESS) includes this file.
//
// There are no vertex buffers or constant buffers. One staging buffer per display holds the display's vertices, indices and
// one draw record per draw. The vertex stage reads its vertex by SV_VertexID: the index (when the draw is indexed), the base
// vertex, then the attributes of the vertex format the D3D11 input layout described. The record holds the same constants the
// D3D11 renderer writes into its two constant buffers (mvp and texgen for the vertex stage, cxmul and cxadd for the pixel
// stage), so both stages compute exactly what the D3D11 shaders compute.
//
// The push block must match FillBlock in src/RenderGraph/Engine/Scaleform/Passes.cpp.

#	include "Common/OrgResources.hlsli"

ORG_PUSH_BLOCK(
	uint GeometryIndex;   // ByteAddressBuffer: this display's vertices, indices and draw records
	uint RecordOffset;    // bytes: this draw's ScaleformFillDraw
	uint Texture0Index;   // Texture2D<float4>: stage 0 (t0)
	uint Texture1Index;   // stage 1 (t1)
	uint Texture2Index;   // stage 2 (t2, YUV only)
	uint Texture3Index;   // stage 3 (t3, YUV only)
	uint Sampler0Index;   // SamplerState: stage 0's (s0)
	uint Sampler1Index;   // s1
	uint Sampler2Index;   // s2
	uint Sampler3Index;   // s3
	uint2 PushPadding;)

/** @brief One draw: the D3D11 renderer's VSConstants and PSConstants, then where its vertices are. Packed as a cbuffer. */
struct ScaleformFillDraw
{
	float4 mvp[4];     // VSConstants.mvp: two rows for the 2D shaders, the matrix's four memory rows for the 3D ones
	float4 texgen[4];  // VSConstants.texgen: stage 0's two rows, then stage 1's
	float4 cxmul;      // PSConstants: the colour transform's multiply, or the solid fill's colour
	float4 cxadd;      // PSConstants: the colour transform's add
	uint vertexOffset;  // bytes: the draw's vertex 0 (its base vertex is added to every index)
	uint vertexStride;  // bytes per vertex: SetVertexData's stride, or 20 for the bitmaps' glyph vertices
	uint indexOffset;   // bytes: the draw's first index
	uint indexSize;     // bytes per index: 2 or 4, or 0 for a draw without indices
	int baseVertex;     // DrawIndexedTriList's base vertex index
	uint3 recordPadding;
};

static ByteAddressBuffer FillGeometry = ResourceDescriptorHeap[GeometryIndex];
static ScaleformFillDraw FillRecord = FillGeometry.Load<ScaleformFillDraw>(RecordOffset);

/** @brief What every fill vertex stage passes on, at fixed locations: each pixel stage reads the ones it uses. */
struct ScaleformFillVaryings
{
	[[vk::location(0)]] float4 color : COLOR0;
	[[vk::location(1)]] float4 factor : COLOR1;
	[[vk::location(2)]] float2 tc0 : TEXCOORD0;
	[[vk::location(3)]] float2 tc1 : TEXCOORD1;
	float4 position : SV_Position;
};

/** @brief The byte address of the vertex SV_VertexID names, through the index list when the draw has one. */
uint ScaleformVertexAddress(uint a_vertexID)
{
	uint index = a_vertexID;
	if (FillRecord.indexSize != 0) {
		const uint address = FillRecord.indexOffset + a_vertexID * FillRecord.indexSize;
		const uint word = FillGeometry.Load(address & ~3u);
		index = FillRecord.indexSize == 4 ? word : ((address & 2u) ? word >> 16 : word & 0xFFFFu);
	}
	return FillRecord.vertexOffset + uint(int(index) + FillRecord.baseVertex) * FillRecord.vertexStride;
}

/** @brief R16G16_SINT at a_address, as the input assembler gives it to an int4: (x, y, 0, 1). */
float4 ScaleformPositionXY16i(uint a_address)
{
	const uint word = FillGeometry.Load(a_address & ~3u);
	const int x = int(word << 16) >> 16;
	const int y = int(word) >> 16;
	return float4(float(x), float(y), 0.0f, 1.0f);
}

/** @brief R8G8B8A8_UNORM at a_address. */
float4 ScaleformColorR8G8B8A8(uint a_address)
{
	const uint word = FillGeometry.Load(a_address);
	return float4(word & 0xFFu, (word >> 8) & 0xFFu, (word >> 16) & 0xFFu, word >> 24) / 255.0f;
}

/** @brief R32G32_FLOAT at a_address, as a float4 input gets it: (x, y, 0, 1). */
float4 ScaleformFloat2(uint a_address)
{
	return float4(asfloat(FillGeometry.Load2(a_address)), 0.0f, 1.0f);
}

#endif

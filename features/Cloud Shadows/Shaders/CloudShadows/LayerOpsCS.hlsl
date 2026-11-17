// Cloud Shadows' layer clears and copies on the render graph (src/Features/CloudShadows/ORGCloudShadows.cpp, "Layer
// operations"). Two passes, one dispatch per tap each, through the client's run gate:
// - LAYER_GATHER: the layer faces that copies read, from their layer into the scratch array's slot the entry names.
// - otherwise (scatter): each layer face that a clear or copy writes, from its scratch slot, or zero for a clear.
// The row is the tap's list of entries. A group's z picks its entry, and the gate's z count is the list's length, so only
// faces with work get groups. Only the ORG path builds it: D3D11 clears and copies the faces itself.
//
// An entry is one word: the layer in bits 24 to 28, the face in bits 16 to 18, the scratch slot (or zero) in the low half.
// Every layer is bound by its whole-array unordered access view, so one dispatch reaches any layer. This shader has no D3D11
// form, so it writes its push block by hand (ORGCloudShadows.cpp, LayerOpsBlock): the 32 layer handles packed in 24 words, the
// scratch's handle, and the row's latch handle and offset. The packing keeps the block within 32 words. Layer i below 24 is
// the low 24 bits of word i. Layer 24 + j is the top bytes of words 3j, 3j + 1 and 3j + 2, low byte first.

cbuffer OrgBindings : register(b0)
{
	uint4 PackedLayers[6];
	uint ScratchHandle;
	uint RowHandle;
	uint RowOffset;
};

// A face a clear writes (scatter only), in an entry's low half.
static const uint kZero = 0xFFFE;

uint PackedWord(uint index)
{
	return PackedLayers[index >> 2][index & 3];
}

uint LayerHandle(uint layer)
{
	if (layer < 24)
		return PackedWord(layer) & 0xFFFFFF;
	const uint first = (layer - 24) * 3;
	return (PackedWord(first) >> 24) | ((PackedWord(first + 1) >> 24) << 8) | ((PackedWord(first + 2) >> 24) << 16);
}

RWTexture2DArray<float> LayerOf(uint layer)
{
	return ResourceDescriptorHeap[NonUniformResourceIndex(LayerHandle(layer))];
}

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	ByteAddressBuffer row = ResourceDescriptorHeap[RowHandle];
	const uint entry = row.Load(RowOffset + dispatchThreadID.z * 4);
	const uint layer = (entry >> 24) & 31;
	const uint face = (entry >> 16) & 7;
	const uint word = entry & 0xFFFF;
	RWTexture2DArray<float> target = LayerOf(layer);
	uint width, height, slices;
	target.GetDimensions(width, height, slices);
	if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
		return;
	const uint3 texel = uint3(dispatchThreadID.xy, face);
#if defined(LAYER_GATHER)
	RWTexture2DArray<float> scratch = ResourceDescriptorHeap[ScratchHandle];
	scratch[uint3(dispatchThreadID.xy, word)] = target[texel];
#else
	Texture2DArray<float> scratch = ResourceDescriptorHeap[ScratchHandle];
	target[texel] = word == kZero ? 0.0f : scratch.Load(int4(dispatchThreadID.xy, word, 0));
#endif
}

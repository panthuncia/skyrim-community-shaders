#include "Common/OrgResources.hlsli"

// Cloud Shadows' layer clears and copies on the render graph (src/Features/CloudShadows/ORGCloudShadows.cpp, "Layer
// operations"). Two passes, one dispatch per layer each, through the client's run gate:
// - LAYER_GATHER: the layer faces that copies read, from the layer into the scratch array's slot the row names.
// - otherwise (scatter): each layer face that a clear or copy writes, from its scratch slot, or zero for a clear.
// A group's z is its face. The row is the layer's: one word per face (six of eight used). Only the ORG path builds it: D3D11
// clears and copies the faces itself.
struct LayerRow
{
	uint4 Faces0123;
	uint4 Faces45;
};

#if defined(LAYER_GATHER)
#	define LAYER_OPS_RESOURCES(X)                  \
		X(SRV, Texture2DArray<float>, Layer, 0)     \
		X(UAV, RWTexture2DArray<float>, Scratch, 0) \
		X(LATCH, LayerRow, Row, 0)
#else
#	define LAYER_OPS_RESOURCES(X)                \
		X(SRV, Texture2DArray<float>, Scratch, 0) \
		X(UAV, RWTexture2DArray<float>, Layer, 0) \
		X(LATCH, LayerRow, Row, 0)
#endif
ORG_PUSH_BLOCK(LAYER_OPS_RESOURCES(ORG_HANDLE))
LAYER_OPS_RESOURCES(ORG_RESOURCE)

// A face with nothing to do, and a face a clear writes (scatter only).
static const uint kNone = 0xFFFFFFFF;
static const uint kZero = 0xFFFFFFFE;

uint FaceWord(uint face)
{
	return face < 4 ? Row.Faces0123[face] : Row.Faces45[face - 4];
}

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	const uint word = FaceWord(dispatchThreadID.z);
	if (word == kNone)
		return;
	uint width, height, faces;
	Layer.GetDimensions(width, height, faces);
	if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
		return;
#if defined(LAYER_GATHER)
	Scratch[uint3(dispatchThreadID.xy, word)] = Layer.Load(int4(dispatchThreadID.xy, dispatchThreadID.z, 0));
#else
	Layer[dispatchThreadID] = word == kZero ? 0.0f : Scratch.Load(int4(dispatchThreadID.xy, word, 0));
#endif
}

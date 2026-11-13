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
// The layer's faces one view each: a single cube has no whole-array SRV, only a one-slice array view per face.
#	define LAYER_OPS_RESOURCES(X)                  \
		X(SRV, Texture2DArray<float>, Face0, 0)     \
		X(SRV, Texture2DArray<float>, Face1, 1)     \
		X(SRV, Texture2DArray<float>, Face2, 2)     \
		X(SRV, Texture2DArray<float>, Face3, 3)     \
		X(SRV, Texture2DArray<float>, Face4, 4)     \
		X(SRV, Texture2DArray<float>, Face5, 5)     \
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

#if defined(LAYER_GATHER)
float LoadFace(uint face, int4 texel)
{
	// Each face view holds its face as its first slice.
	switch (face) {
	case 0:
		return Face0.Load(texel);
	case 1:
		return Face1.Load(texel);
	case 2:
		return Face2.Load(texel);
	case 3:
		return Face3.Load(texel);
	case 4:
		return Face4.Load(texel);
	default:
		return Face5.Load(texel);
	}
}
#endif

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	const uint word = FaceWord(dispatchThreadID.z);
	if (word == kNone)
		return;
	uint width, height, slices;
#if defined(LAYER_GATHER)
	Face0.GetDimensions(width, height, slices);
#else
	Layer.GetDimensions(width, height, slices);
#endif
	if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
		return;
#if defined(LAYER_GATHER)
	Scratch[uint3(dispatchThreadID.xy, word)] = LoadFace(dispatchThreadID.z, int4(dispatchThreadID.xy, 0, 0));
#else
	Layer[dispatchThreadID] = word == kZero ? 0.0f : Scratch.Load(int4(dispatchThreadID.xy, word, 0));
#endif
}

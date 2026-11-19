// Drawcall Limit Fix. The frame's per-object pixel rows.
// MaterialData's LOD and alpha fades, EmitColor and SSRParams.w are per draw, not per pixel, so this computes them once a frame for
// every object slot, first in the depth segment. The pixel stages read the rows at t116. Inputs are read as the draws read them.
// The backing comes from the latch, so a recording kept across frames writes each frame's rows.

#include "Common/DeviceRef.hlsli"

#define DCLF_BINDLESS
#include "Common/DCLFObjects.hlsli"

// Must match FadePixelRows.cpp's Constants.
cbuffer PixelRowsConstants : register(b0)
{
	uint LatchIndex;       // ByteAddressBuffer, the segment's BuildDrawsLatch block
	uint LatchOffset;      // its latch in this frame slot
	uint2 RecordTextures;  // the frame record's texture registers by buffer view ref
	uint2 LodFadeFrame;    // the frame's LodFadeFrame in the frame constants, by buffer view ref
	uint2 Padding;
}

// BuildDrawsLatch's frameRowsRef, two words, and frameRowsCount.
static const uint kLatchFrameRows = 356;
static const uint kPixelObjectRowBytes = 48;  // sizeof(DCLFPixelObjectRow)
// Must match numthreads times kGroups in FadePixelRows.cpp.
static const uint kThreads = 64 * 64;

// A texture register's descriptor in the frame record.
uint RecordTexture(uint a_register) { return DeviceLoad(uint, DeviceRefAdd(RecordTextures, 4u * a_register), 4); }

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint3 rows = latch.Load3(LatchOffset + kLatchFrameRows);
	const DeviceRef base = rows.xy;
	if (DeviceRefIsNull(base))
		return;
	StructuredBuffer<DCLFObjectRecord> objects = ResourceDescriptorHeap[RecordTexture(127)];
	StructuredBuffer<DCLFPlacement> placements = ResourceDescriptorHeap[RecordTexture(123)];
	StructuredBuffer<DCLFShadingRow> shading = ResourceDescriptorHeap[RecordTexture(121)];
	StructuredBuffer<DCLFFadeNodeState> states = ResourceDescriptorHeap[RecordTexture(120)];
	StructuredBuffer<DCLFFadeRootStatic> roots = ResourceDescriptorHeap[RecordTexture(119)];
	StructuredBuffer<DCLFFadeRootStatic> seeds = ResourceDescriptorHeap[RecordTexture(117)];
	uint objectCount, placementCount, stride;
	objects.GetDimensions(objectCount, stride);
	placements.GetDimensions(placementCount, stride);
	const uint known = min(objectCount, placementCount);
	const DCLFLodFadeFrameData frame = DeviceLoad(DCLFLodFadeFrameData, LodFadeFrame, 16);
	[loop] for (uint object = id.x; object < rows.z; object += kThreads) {
		// A slot past the payload's objects or the frame's placements is no draw's, so its row is 0.
		DCLFPixelObjectRow row = (DCLFPixelObjectRow)0;
		if (object < known)
			row = DCLFPixelObjectRowOf(objects, shading, placements, roots, states, seeds, frame, object);
		// Three float4s, not the struct. AMD's legacy Windows driver (Polaris, 2.0.279) crashes compiling a struct store to an address.
		const DeviceRef at = DeviceRefAdd(base, object * kPixelObjectRowBytes);
		DeviceStore(float4, at, row.MaterialData, 16);
		DeviceStore(float4, DeviceRefAdd(at, 16), row.EmitColorSSR, 16);
		DeviceStore(float4, DeviceRefAdd(at, 32), float4(row.CopyAlpha, row.AlphaTestRef, row.Padding), 16);
	}
}

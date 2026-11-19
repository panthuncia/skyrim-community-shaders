// Drawcall Limit Fix. Writes the frame's texture transforms into the frame's copy of the animated material rows.
// The installed rows are immutable versions shared by frames in flight, so they are never written here. Phase 0 copies every
// animated row whole into the frame's copy. Phase 1 stores the frame's TexcoordOffset values into the copies, in the depth epoch
// before any draw reads them. Every row and entry is written every frame, so the copy is correct whatever backing the pool gave.

// Must match TextureTransformsPass.cpp's Constants.
#include "Common/DeviceRef.hlsli"

cbuffer TextureTransformsConstants : register(b0)
{
	uint LatchIndex;   // ByteAddressBuffer, the depth segment's BuildDrawsLatch block
	uint LatchOffset;  // its latch in this frame slot
	uint Phase;        // 0 copies the animated rows, 1 stores the entries into the copies
	uint Padding;
}

// Row 0 is the header, with the entry count in RowOffset and the animated row count in Mask. Entries follow, then the animated rows.
struct TextureTransformRow
{
	uint RowOffset;  // bytes past the material rows' address
	uint Mask;       // the components to store, bit c
	uint2 RowPadding;
	float4 Values;
};

// BuildDrawsLatch words from GpuLayouts.h.
static const uint kLatchStageWord = 300;
static const uint kLatchMaterialRowsRef = 384;
static const uint kLatchAnimatedRowsRef = 392;
// DrawPipelines.h's kMaterialRowBytes and kMaterialRowHeader.
static const uint kMaterialRowBytes = 1024;
static const uint kMaterialRowHeader = 768;
static const uint kNoAnimatedRow = 0xFFFFFFFFu;
// Must match numthreads times kGroups in TextureTransformsPass.cpp.
static const uint kThreads = 64 * 32;

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint transformsIndex = latch.Load(LatchOffset + kLatchStageWord);
	// The pass ran, with this stage word, in this phase.
	DEVICE_REF_TRACE_VALUE(uint2(transformsIndex, Phase));
	if (transformsIndex == 0)
		return;  // no producer this frame, so the draws read the installed rows
	const DeviceRef base = latch.Load2(LatchOffset + kLatchMaterialRowsRef);
	const DeviceRef copyBase = latch.Load2(LatchOffset + kLatchAnimatedRowsRef);
	StructuredBuffer<TextureTransformRow> transforms = ResourceDescriptorHeap[transformsIndex];
	// Null refs make the counts 0, with no early return. AMD's RDNA2 Windows compiler took the `||` null guard as base.x == 0 past it,
	// so every load read heap slot 0, which once caused a device fault, and every store wrote slot 1.
	const bool rowsValid = !DeviceRefIsNull(base) && !DeviceRefIsNull(copyBase);
	const uint count = rowsValid ? transforms[0].RowOffset : 0;
	const uint animated = rowsValid ? transforms[0].Mask : 0;
	// The entries and animated rows the header counts.
	DEVICE_REF_TRACE_VALUE(uint2(count, animated));
	if (Phase == 0) {
		// Each animated row whole, one 16-byte word per thread.
		const uint kWords = kMaterialRowBytes / 16;
		[loop] for (uint w = id.x; w < animated * kWords; w += kThreads) {
			const uint k = w / kWords;
			const uint installed = transforms[count + 1 + k].RowOffset;
			if (installed == kNoAnimatedRow)
				continue;  // a free row, no header names it
			const uint at = (w % kWords) * 16;
			DeviceStore(uint4, DeviceRefAdd(copyBase, k * kMaterialRowBytes + at), DeviceLoad(uint4, DeviceRefAdd(base, installed + at), 16), 16);
		}
		return;
	}
	[loop] for (uint i = id.x; i < count; i += kThreads) {
		const TextureTransformRow row = transforms[i + 1];
		// The entry's row's copy, named by its installed header.
		const uint rowBase = row.RowOffset / kMaterialRowBytes * kMaterialRowBytes;
		const uint k = DeviceLoad(uint, DeviceRefAdd(base, rowBase + kMaterialRowHeader), 4);
		// kNoAnimatedRow is past every count. One unsigned compare, no equality test. AMD's RDNA2 Windows compiler took
		// `k == kNoAnimatedRow || k >= animated` as k == kNoAnimatedRow past it, and stored every entry ~4 TB past the copy.
		if (k >= animated)
			continue;
		const DeviceRef at = DeviceRefAdd(copyBase, k * kMaterialRowBytes + (row.RowOffset - rowBase));
		[unroll] for (uint c = 0; c < 4; ++c)
			if ((row.Mask >> c) & 1)
				DeviceStore(uint, DeviceRefAdd(at, c * 4), asuint(row.Values[c]), 4);
	}
}

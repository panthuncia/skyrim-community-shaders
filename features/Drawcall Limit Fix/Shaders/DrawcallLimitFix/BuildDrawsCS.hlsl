// Drawcall Limit Fix: writes the indirect draw sequences (DrawSequence, Records.h) of this frame's draws
// from the per-draw inputs and the geometry table, appending them with an atomic count. Compiled to SPIR-V
// for the render graph (BasicRHI's descriptor-heap ABI); buffers are fetched from the descriptor heap by index.

cbuffer BuildDrawsConstants : register(b0)
{
	uint DrawCount;
	uint InputsIndex;      // ByteAddressBuffer: DrawInput[DrawCount]
	uint GeometriesIndex;  // ByteAddressBuffer: GeometryDraw[]
	uint SequencesIndex;   // RWByteAddressBuffer: DrawSequence[]
	uint CountIndex;       // RWByteAddressBuffer: uint (zeroed before the dispatch)
	uint RecordsAddressLo;  // device address of DrawBindings[0]
	uint RecordsAddressHi;
	uint RecordStride;
}

// DrawInput: 16 bytes.
static const uint kInputStride = 16;
// GeometryDraw: 40 bytes (vertex buffer view, index buffer view, index count, first index).
static const uint kGeometryStride = 40;
// DrawSequence: 64 bytes, 4-byte packed.
static const uint kSequenceStride = 64;
static const uint kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	const uint draw = dispatchID.x;
	if (draw >= DrawCount)
		return;

	ByteAddressBuffer inputs = ResourceDescriptorHeap[InputsIndex];
	ByteAddressBuffer geometries = ResourceDescriptorHeap[GeometriesIndex];
	RWByteAddressBuffer sequences = ResourceDescriptorHeap[SequencesIndex];
	RWByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];

	const uint4 input = inputs.Load4(draw * kInputStride);  // pipeline index, record index, geometry index, flags
	const uint geometryOffset = input.z * kGeometryStride;
	const uint4 vertexBuffer = geometries.Load4(geometryOffset);        // address lo, hi, size, stride
	const uint4 indexBuffer = geometries.Load4(geometryOffset + 16);    // address lo, hi, size, index count
	const uint firstIndex = geometries.Load(geometryOffset + 32);

	// 64-bit record address = RecordsAddress + record index * RecordStride.
	const uint recordOffset = input.y * RecordStride;
	const uint recordLo = RecordsAddressLo + recordOffset;
	const uint recordHi = RecordsAddressHi + (recordLo < RecordsAddressLo ? 1 : 0);

	uint slot;
	count.InterlockedAdd(0, 1, slot);
	const uint base = slot * kSequenceStride;
	sequences.Store(base + 0, input.x);
	sequences.Store2(base + 4, uint2(recordLo, recordHi));
	sequences.Store4(base + 12, vertexBuffer);
	sequences.Store4(base + 28, uint4(indexBuffer.xyz, kIndexFormatR16));
	sequences.Store4(base + 44, uint4(indexBuffer.w, 1, firstIndex, 0));
	sequences.Store(base + 60, 0);
}

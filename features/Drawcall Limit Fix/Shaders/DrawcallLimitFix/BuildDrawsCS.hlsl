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
	// Phase 4 culling. ViewProj is the main pass's, so it is camera-relative: the bounds are absolute
	// world space and Eye (the camera's posAdjust) is subtracted before projecting them.
	uint CullMode;  // 0 off, 1 frustum
	float3 Eye;
	row_major float4x4 ViewProj;  // as the engine stores it (row 2 is the z row, row 3 the w row)
}

// DrawInput: 32 bytes (pipeline/record/geometry/flags, then the world-space bounding sphere).
static const uint kInputStride = 32;
// GeometryDraw: 40 bytes (vertex buffer view, index buffer view, index count, first index).
static const uint kGeometryStride = 40;
// DrawSequence: 64 bytes, 4-byte packed.
static const uint kSequenceStride = 64;
static const uint kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT

// The bounding sphere's world-space AABB, projected corner by corner. The box contains the sphere, so
// every test built on it errs towards keeping the object: an object is only rejected when all eight
// corners are outside the same clip plane, which no visible object can be.
//
// A corner behind the near plane makes the projection meaningless, so the object is kept.
bool Culled(float3 boundCentre, float boundRadius)
{
	const float3 centre = boundCentre - Eye;
	float4 planes = float4(1, 1, 1, 1);  // all-corners-outside accumulators: -x, +x, -y, +y
	float2 depthPlanes = float2(1, 1);   // near, far
	[unroll] for (uint corner = 0; corner < 8; ++corner) {
		const float3 offset = float3((corner & 1) ? boundRadius : -boundRadius,
			(corner & 2) ? boundRadius : -boundRadius,
			(corner & 4) ? boundRadius : -boundRadius);
		const float4 clip = mul(ViewProj, float4(centre + offset, 1.0));
		if (clip.w <= 1e-4)
			return false;  // crosses the near plane: treat as visible
		planes.x *= (clip.x < -clip.w) ? 1 : 0;
		planes.y *= (clip.x > clip.w) ? 1 : 0;
		planes.z *= (clip.y < -clip.w) ? 1 : 0;
		planes.w *= (clip.y > clip.w) ? 1 : 0;
		depthPlanes.x *= (clip.z < 0) ? 1 : 0;
		depthPlanes.y *= (clip.z > clip.w) ? 1 : 0;
	}
	return any(planes != 0) || any(depthPlanes != 0);
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	const uint draw = dispatchID.x;
	if (draw >= DrawCount)
		return;

	ByteAddressBuffer inputs = ResourceDescriptorHeap[InputsIndex];
	ByteAddressBuffer geometries = ResourceDescriptorHeap[GeometriesIndex];
	RWByteAddressBuffer sequences = ResourceDescriptorHeap[SequencesIndex];
	RWByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];

	const uint inputOffset = draw * kInputStride;
	const uint4 input = inputs.Load4(inputOffset);  // pipeline index, record index, geometry index, flags
	if (CullMode != 0) {
		uint tested;
		count.InterlockedAdd(8, 1, tested);  // word 2: how many the culling looked at, so that a count of
		                                     // zero rejections can be told apart from culling not running
		const float4 bound = asfloat(inputs.Load4(inputOffset + 16));  // centre (world), radius
		if (Culled(bound.xyz, bound.w)) {
			uint culled;
			count.InterlockedAdd(4, 1, culled);  // word 1: how many this frame's culling rejected
			return;
		}
	}
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

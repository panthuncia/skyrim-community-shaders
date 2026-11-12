#ifndef __DEVICE_REF_DEPENDENCY_HLSL__
#define __DEVICE_REF_DEPENDENCY_HLSL__

// Loads and stores through a buffer view ref (BasicRHI's Device::GetBufferViewRef, rhi_shader_abi.h BUFFER_VIEW_REF_*). The same
// code on SPIR-V (Vulkan) and DXIL (D3D12): no buffer pointers.
//
// A ref is two words: x the heap slot of the buffer's raw SRV, y a byte offset into the buffer. The buffer's raw UAV is at slot
// x + 1 (a null UAV for a buffer without UAV access: a store is dropped). A ref with x 0 names no buffer; nothing makes a load
// through it safe, so test it first (DeviceRefIsNull).
//
//   DeviceLoad(T, ref[, alignment])          a T read at ref
//   DeviceStore(T, ref, value[, alignment])  value written at ref
//   DeviceRefAdd(ref, bytes)                 the ref bytes further on
//   DeviceRefFromVA(base, baseVA, va)        a ref into a buffer from a device address in it, given the buffer's ref and address
//
// Words a pipeline layout or the GPU front end reads (pushed and indirect block addresses, index buffer views) stay device
// addresses; a shader that also reads through one turns it into a ref with DeviceRefFromVA. The alignment argument is ignored.
// FXC (the D3D11 builds) sees none of this: only DXC builds load through refs.
// The macros may evaluate the ref more than once: pass expressions without side effects.

#if defined(__spirv__) || defined(__HLSL_VERSION)

#	define DeviceRef uint2

DeviceRef DeviceRefAdd(DeviceRef a_ref, uint a_bytes) { return DeviceRef(a_ref.x, a_ref.y + a_bytes); }
bool DeviceRefIsNull(DeviceRef a_ref) { return a_ref.x == 0; }

// a_va is in the buffer whose ref is a_base and whose device address is a_baseVA (each two words, low first); 0 gives the null ref.
// A buffer is under 4 GiB, so the low words' difference is the offset.
DeviceRef DeviceRefFromVA(DeviceRef a_base, uint2 a_baseVA, uint2 a_va)
{
	return all(a_va == 0) ? DeviceRef(0, 0) : DeviceRef(a_base.x, a_base.y + (a_va.x - a_baseVA.x));
}

ByteAddressBuffer DeviceRefBuffer(DeviceRef a_ref)
{
	return ResourceDescriptorHeap[NonUniformResourceIndex(a_ref.x)];
}

RWByteAddressBuffer DeviceRefRWBuffer(DeviceRef a_ref)
{
	return ResourceDescriptorHeap[NonUniformResourceIndex(a_ref.x + 1)];
}

#	if defined(DEVICE_REF_TRACE)
// CS_DCLF_REF_TRACE (DCLF's RefTrace.cpp): DEVICE_REF_TRACE is the trace buffer's view slot. Each load or store site (its line) records
// the refs it uses into the buffer's table, which the CPU logs as "ref trace:" lines. An entry is four words: the key (the line, the
// stage, the kind), the first ref seen (two words), the hit count. The kinds: 0 a ref with a slot, 1 a null ref, 2 an odd slot (a UAV
// slot read as a ref), 3 a descriptor index of 0 read from a row (DEVICE_REF_TRACE_ZERO_INDEX).
// Only sampled lanes record, one a wave: a stage's main sets DeviceRefTraceLane (DEVICE_REF_TRACE_SAMPLE).
static const uint kDeviceRefTraceEntries = 1024;
static bool DeviceRefTraceLane = true;
#		if defined(PSHADER)
static const uint kDeviceRefTraceStage = 1;
#		elif defined(VSHADER)
static const uint kDeviceRefTraceStage = 0;
#		else
static const uint kDeviceRefTraceStage = 2;
#		endif
// Records a_sample under the key (a_line, a_store, the stage, a_kind) from a sampled lane, one a wave.
void DeviceRefTraceNote(uint2 a_sample, uint a_line, uint a_store, uint a_kind)
{
	[branch] if (DeviceRefTraceLane && WaveIsFirstLane())
	{
		RWByteAddressBuffer trace = ResourceDescriptorHeap[NonUniformResourceIndex(DEVICE_REF_TRACE + 1)];
		const uint key = (a_line << 8) | (a_store << 6) | (kDeviceRefTraceStage << 4) | a_kind;
		uint slot = (key * 2654435761u) >> 22;  // the top 10 bits: kDeviceRefTraceEntries
		for (uint probe = 0; probe < 8; ++probe, slot = (slot + 1) & (kDeviceRefTraceEntries - 1)) {
			uint previous;
			trace.InterlockedCompareExchange(16 * slot, 0u, key, previous);
			if (previous == 0u)
				trace.Store2(16 * slot + 4, a_sample);
			if (previous == 0u || previous == key) {
				trace.InterlockedAdd(16 * slot + 12, 1u);
				break;
			}
		}
	}
}
DeviceRef DeviceRefTraced(DeviceRef a_ref, uint a_line, uint a_store)
{
	DeviceRefTraceNote(a_ref, a_line, a_store, a_ref.x == 0 ? 1u : ((a_ref.x & 1u) != 0 ? 2u : 0u));
	return a_ref;
}
// A descriptor index read from a row that is 0 (kind 3: no texture or sampler), with what names it (a register, an offset).
#		define DEVICE_REF_TRACE_ZERO_INDEX(a_index, a_what) 			if ((a_index) == 0u)                             			DeviceRefTraceNote(a_what, __LINE__, 0, 3)
#		define DEVICE_REF_TRACE_SAMPLE(a_value) DeviceRefTraceLane = ((a_value) & 63u) == 0u
#		define DeviceLoad(T, a_ref, ...) DeviceRefBuffer(DeviceRefTraced(a_ref, __LINE__, 0)).Load<T>((a_ref).y)
#		define DeviceStore(T, a_ref, a_value, ...) DeviceRefRWBuffer(DeviceRefTraced(a_ref, __LINE__, 1)).Store<T>((a_ref).y, a_value)
#	else
#		define DEVICE_REF_TRACE_SAMPLE(a_value)
#		define DEVICE_REF_TRACE_ZERO_INDEX(a_index, a_what)
#		define DeviceLoad(T, a_ref, ...) DeviceRefBuffer(a_ref).Load<T>((a_ref).y)
#		define DeviceStore(T, a_ref, a_value, ...) DeviceRefRWBuffer(a_ref).Store<T>((a_ref).y, a_value)
#	endif

#endif

#endif  // __DEVICE_REF_DEPENDENCY_HLSL__

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

#	define DeviceLoad(T, a_ref, ...) DeviceRefBuffer(a_ref).Load<T>((a_ref).y)
#	define DeviceStore(T, a_ref, a_value, ...) DeviceRefRWBuffer(a_ref).Store<T>((a_ref).y, a_value)

#endif

#endif  // __DEVICE_REF_DEPENDENCY_HLSL__

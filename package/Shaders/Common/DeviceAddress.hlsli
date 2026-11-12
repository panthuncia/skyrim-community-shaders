#ifndef __DEVICE_ADDRESS_DEPENDENCY_HLSL__
#define __DEVICE_ADDRESS_DEPENDENCY_HLSL__

// Loads and stores through a 64-bit buffer device address (RenderGraph's GetBufferDeviceAddress), on both of the render graph's
// backends:
//
//   DeviceLoad(T, address[, alignment])         a T read at address
//   DeviceStore(T, address, value[, alignment]) value written at address
//
// SPIR-V (Vulkan): exactly vk::RawBufferLoad<T>(...) and vk::RawBufferStore<T>(...), so a shader compiles to the same code as
// one that calls them itself.
//
// DXIL (D3D12) has no buffer pointers. BasicRHI's D3D12 backend keeps a table of the buffers whose addresses were asked for
// (rhi_shader_abi.h, D3D12_DEVICE_ADDRESS_*): a page table from 64 KiB address pages to entries, and per entry the buffer's
// base, size and raw views. A load or store finds the entry that holds the address and reads or writes its raw view at
// address - base. An address in no buffer reads the heap's first descriptor; the alignment argument is ignored.
// FXC (the D3D11 builds) sees neither: only DXC builds load through addresses.
// The macros may evaluate the address more than once: pass expressions without side effects.

#if defined(__spirv__)

#	define DeviceLoad(T, ...) vk::RawBufferLoad<T>(__VA_ARGS__)
#	define DeviceStore(T, ...) vk::RawBufferStore<T>(__VA_ARGS__)

#elif defined(__HLSL_VERSION)  // DXC's DXIL; FXC builds (D3D11) never load through addresses and see nothing here

// rhi_shader_abi.h: D3D12_DEVICE_ADDRESS_TABLE_SLOT, _ENTRY_BYTES, _PAGE_BITS, _PAGE_SLOTS (2^18), _PROBES.
static const uint kDeviceAddressTableSlot = 1000000;
static const uint kDeviceAddressEntryBytes = 32;
static const uint kDeviceAddressPageBits = 16;
static const uint kDeviceAddressPageSlots = 1u << 18;
static const uint kDeviceAddressProbes = 64;

// The page table's first slot for a page: BasicRHI's Dx12AddressPageSlot.
uint DeviceAddressPageSlot(uint64_t a_page)
{
	const uint mixed = (uint(a_page) ^ uint(a_page >> 32)) * 0x9E3779B1u;
	return mixed >> 14;  // 32 - log2(kDeviceAddressPageSlots)
}

// The entry whose buffer holds a_address, or 0xFFFFFFFF.
uint DeviceAddressEntry(uint64_t a_address)
{
	ByteAddressBuffer entries = ResourceDescriptorHeap[kDeviceAddressTableSlot];
	ByteAddressBuffer pages = ResourceDescriptorHeap[kDeviceAddressTableSlot + 1];
	const uint64_t page = a_address >> kDeviceAddressPageBits;
	uint slot = DeviceAddressPageSlot(page);
	for (uint probe = 0; probe < kDeviceAddressProbes; ++probe) {
		const uint4 entry = pages.Load4(slot * 16);  // entry, unused, page (low, high)
		const uint64_t key = (uint64_t(entry.w) << 32) | uint64_t(entry.z);
		if (key == 0)
			break;
		if (key == page && entry.x != 0xFFFFFFFFu) {
			const uint4 range = entries.Load4(entry.x * kDeviceAddressEntryBytes);  // base, size
			const uint64_t base = (uint64_t(range.y) << 32) | uint64_t(range.x);
			const uint64_t size = (uint64_t(range.w) << 32) | uint64_t(range.z);
			if (a_address >= base && a_address - base < size)
				return entry.x;
		}
		slot = (slot + 1) & (kDeviceAddressPageSlots - 1);
	}
	return 0xFFFFFFFFu;
}

// The raw views and the offset of a_address in its buffer.
uint2 DeviceAddressViews(uint64_t a_address)
{
	ByteAddressBuffer entries = ResourceDescriptorHeap[kDeviceAddressTableSlot];
	const uint entry = DeviceAddressEntry(a_address);
	return entry != 0xFFFFFFFFu ? entries.Load2(entry * kDeviceAddressEntryBytes + 16) : uint2(0, 0);
}

uint DeviceAddressOffset(uint64_t a_address)
{
	ByteAddressBuffer entries = ResourceDescriptorHeap[kDeviceAddressTableSlot];
	const uint entry = DeviceAddressEntry(a_address);
	if (entry == 0xFFFFFFFFu)
		return 0;
	const uint2 base = entries.Load2(entry * kDeviceAddressEntryBytes);
	return uint(a_address - ((uint64_t(base.y) << 32) | uint64_t(base.x)));
}

// The raw view and the offset of a_address in its buffer, from one lookup: for a block read many times (DCLF's frame record).
uint2 DeviceAddressSpan(uint64_t a_address)
{
	ByteAddressBuffer entries = ResourceDescriptorHeap[kDeviceAddressTableSlot];
	const uint entry = DeviceAddressEntry(a_address);
	if (entry == 0xFFFFFFFFu)
		return uint2(0, 0);
	const uint2 base = entries.Load2(entry * kDeviceAddressEntryBytes);
	const uint view = entries.Load(entry * kDeviceAddressEntryBytes + 16);
	return uint2(view, uint(a_address - ((uint64_t(base.y) << 32) | uint64_t(base.x))));
}

ByteAddressBuffer DeviceAddressBuffer(uint64_t a_address)
{
	return ResourceDescriptorHeap[NonUniformResourceIndex(DeviceAddressViews(a_address).x)];
}

RWByteAddressBuffer DeviceAddressRWBuffer(uint64_t a_address)
{
	return ResourceDescriptorHeap[NonUniformResourceIndex(DeviceAddressViews(a_address).y)];
}

#	define DeviceLoad(T, a_address, ...) DeviceAddressBuffer(a_address).Load<T>(DeviceAddressOffset(a_address))
#	define DeviceStore(T, a_address, a_value, ...) DeviceAddressRWBuffer(a_address).Store<T>(DeviceAddressOffset(a_address), a_value)

#endif

#endif  // __DEVICE_ADDRESS_DEPENDENCY_HLSL__

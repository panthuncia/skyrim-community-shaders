// Drawcall Limit Fix. The shadow views' index pool. A group copies one listed index buffer word by word into its pool range,
// so the shadow views' plain indexed draws bind the pool alone.

// Must match IndexPoolConstants in GpuLayouts.h.
#include "Common/DeviceRef.hlsli"

cbuffer IndexPoolConstants : register(b0)
{
	uint LatchIndex;   // ByteAddressBuffer, the shadow latch block
	uint LatchOffset;  // the copies' dispatch, groups x, y, z, then the copy count
	uint CopiesIndex;  // StructuredBuffer<uint4>, per copy the source ref, the range's first index, its words
	uint PoolIndex;    // RWByteAddressBuffer: the pool's 16-bit indices
}

static const uint kGroupsX = 65535;  // the dispatch's x limit, a copy past it is in the next row

[numthreads(64, 1, 1)] void main(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint copy = group.y * kGroupsX + group.x;
	if (copy >= latch.Load(LatchOffset + 12))
		return;
	StructuredBuffer<uint4> copies = ResourceDescriptorHeap[CopiesIndex];
	RWByteAddressBuffer pool = ResourceDescriptorHeap[PoolIndex];
	const uint4 job = copies[copy];
	const DeviceRef source = job.xy;
	// A range starts at an even index. The source's last word may hold two bytes past its indices, which land in the range's own padding.
	[loop] for (uint word = thread; word < job.w; word += 64)
		pool.Store(job.z * 2 + word * 4, DeviceLoad(uint, DeviceRefAdd(source, word * 4)));
}

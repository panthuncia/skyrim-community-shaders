// Drawcall Limit Fix. Builds the hierarchical depth buffer that occlusion culling tests against.
//
// The HZB holds raw NDC depth. Skyrim's z_ndc = A - B/w is monotonic in distance and the test only compares order, so linearising
// would add a step that can go wrong for no gain. Each level is the MAXIMUM of the one below, so a texel holds the farthest surface
// under it. That keeps the test conservative, since a footprint with some sky never culls.
// AMD's single-pass downsampler writes every mip in one dispatch, which needs a power-of-two domain. Mip 0 is half the next power
// of two of the depth. Padding inside the domain loads as 1.0, the far plane, which suppresses culling. Texels outside the domain
// or a non-square level load as 0.0, the reduction's identity. Stores outside a level are dropped.
// A domain wider than 4096 overflows the last group's 64x64 at mip 5, so HzbPass splits the chain into several dispatches.

// Must match HzbConstants in HzbLayout.h.
cbuffer HzbConstants : register(b0)
{
	uint SourceIndex;   // FromDepth reads the scene depth Texture2D, otherwise an HZB level RWTexture2D
	uint FromDepth;
	uint2 ValidSize;    // the source texels holding depth
	uint2 DomainSize;   // the source's extent, of which [ValidSize, DomainSize) is padding
	uint2 TargetSize;   // the first written level's size, level i is max(1, TargetSize >> i)
	uint Mips;          // the levels this dispatch writes
	uint WorkGroups;    // the dispatch's group count, for the last-group test
	uint CounterIndex;  // RWStructuredBuffer<uint>, finished groups, zeroed again by the last
	uint Padding;
	uint4 TargetIndices[3];  // RWTexture2D<float> per written level, 12 at most
}

static const float kFarPlane = 1.0;
static const float kNoArea = 0.0;

#define A_GPU
#define A_HLSL
#include "DrawcallLimitFix/FidelityFX/ffx_a.h"

groupshared AU1 spdCounter;
groupshared AF1 spdIntermediate[16][16];

uint TargetIndex(uint a_level)
{
	return TargetIndices[a_level >> 2][a_level & 3];
}

bool InsideLevel(int2 a_position, uint a_level)
{
	const int2 size = int2(max(TargetSize >> a_level, uint2(1, 1)));
	return all(a_position >= 0) && all(a_position < size);
}

AF4 SpdLoadSourceImage(ASU2 p, AU1 slice)
{
	if (any(p < 0) || any(p >= int2(DomainSize)))
		return kNoArea.xxxx;
	if (any(p >= int2(ValidSize)))
		return kFarPlane.xxxx;
	if (FromDepth != 0) {
		Texture2D<float> depth = ResourceDescriptorHeap[SourceIndex];
		return depth.Load(int3(p, 0)).xxxx;
	}
	RWTexture2D<float> source = ResourceDescriptorHeap[SourceIndex];
	return source[p].xxxx;
}

// The last group's reads of level 5, written by every other group.
AF4 SpdLoad(ASU2 p, AU1 slice)
{
	if (!InsideLevel(p, 5))
		return kNoArea.xxxx;
	globallycoherent RWTexture2D<float> level = ResourceDescriptorHeap[TargetIndex(5)];
	return level[p].xxxx;
}

void SpdStore(ASU2 p, AF4 value, AU1 mip, AU1 slice)
{
	if (!InsideLevel(p, mip))
		return;
	if (mip == 5) {
		globallycoherent RWTexture2D<float> level = ResourceDescriptorHeap[TargetIndex(5)];
		level[p] = value.x;
		return;
	}
	RWTexture2D<float> level = ResourceDescriptorHeap[NonUniformResourceIndex(TargetIndex(mip))];
	level[p] = value.x;
}

AF4 SpdReduce4(AF4 v0, AF4 v1, AF4 v2, AF4 v3)
{
	return max(max(v0.x, v1.x), max(v2.x, v3.x)).xxxx;
}

void SpdIncreaseAtomicCounter(AU1 slice)
{
	globallycoherent RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[CounterIndex];
	// This thread's level 5 store must be visible before the group arrives, and every arrival before the last group reads.
	DeviceMemoryBarrier();
	InterlockedAdd(counter[0], 1, spdCounter);
	DeviceMemoryBarrier();
}

AU1 SpdGetAtomicCounter()
{
	return spdCounter;
}

void SpdResetAtomicCounter(AU1 slice)
{
	globallycoherent RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[CounterIndex];
	counter[0] = 0;
}

AF4 SpdLoadIntermediate(AU1 x, AU1 y)
{
	return spdIntermediate[x][y].xxxx;
}

void SpdStoreIntermediate(AU1 x, AU1 y, AF4 value)
{
	spdIntermediate[x][y] = value.x;
}

#include "DrawcallLimitFix/FidelityFX/ffx_spd.h"

[numthreads(256, 1, 1)] void main(uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	SpdDownsample(AU2(groupID.xy), AU1(localIndex), AU1(Mips), AU1(WorkGroups), AU1(0));
}

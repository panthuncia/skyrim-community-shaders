// Drawcall Limit Fix. Sorts the forward range back to front. BuildDrawsCS staged each visible forward draw with its sort key in
// three words. Each thread ranks one staged draw in the engine's blended order and writes it to that rank of the sorted half,
// restoring the three words. Every sorted slot below the count is written exactly once.
//
// The engine's order (FUN_1414b39d0) sorts the blended list by view distance, farthest first (FUN_1414e8fc0, stable), and merges
// in ordered node groups by their node's distance (0x1414f55e0). A group draws whole in child order, before a nearer list pass
// and after one at its distance or farther (FUN_1414f5330). Ties break by group, ordinal, object index and first index, so the
// order is the same every frame whatever the atomic append gave.
//
// The rank is O(n^2) with no cap, reading keys through groupshared memory a tile of 64 at a time. A much larger class would need
// a radix or bitonic sort with the same contract. Each state class sorts in its own dispatch, so order across classes is the classes'.

// Must match ForwardSortConstants in Main/FrameLayout.h.
cbuffer ForwardSortConstants : register(b0)
{
	uint SequencesIndex;  // RWByteAddressBuffer: DrawSequence[], the main sequences
	uint CountIndex;      // ByteAddressBuffer, the count words
	uint StagingBase;     // the staging half's first slot, the sorted half follows Capacity slots on
	uint Capacity;        // each half's slots
	uint SequenceStride;  // bytes
	uint CountOffset;     // with kSortCountOffset, the count word's byte offset in CountIndex
	uint Options;         // kSortCountOffset, kSortRankInstance
	uint Padding2;
}

static const uint kSortCountOffset = 1;   // ForwardSortConstants::kCountOffset
static const uint kSortRankInstance = 2;  // ForwardSortConstants::kRankInstance, firstInstance is the rank

static const uint kCountForward = 128;     // GpuLayouts.h, kCountForwardWord
static const uint kObjectWordOffset = 20;  // DrawSequence::objectIndex
static const uint kInstanceCountOffset = 120;
static const uint kFirstIndexOffset = 124;  // the DrawIndexed arguments' firstIndex
static const uint kVertexOffsetOffset = 128;
static const uint kFirstInstanceOffset = 132;
static const uint kObjectIndexMask = (1u << 26) - 1;  // Records.h, kObjectIndexMask
static const uint kTile = 64;                         // the thread group, IndirectDraws' kForwardSortGroup

struct SortKey
{
	float depth;
	uint group;
	uint ordinal;  // 0 for a plain draw
	uint object;
	uint first;
};

SortKey LoadKey(RWByteAddressBuffer a_sequences, uint a_slot)
{
	const uint at = (StagingBase + a_slot) * SequenceStride;
	SortKey key;
	key.depth = asfloat(a_sequences.Load(at + kFirstInstanceOffset));
	key.group = a_sequences.Load(at + kInstanceCountOffset);
	key.ordinal = a_sequences.Load(at + kVertexOffsetOffset);
	key.object = a_sequences.Load(at + kObjectWordOffset) & kObjectIndexMask;
	key.first = a_sequences.Load(at + kFirstIndexOffset);
	return key;
}

// Whether draw a comes before draw b in the blended order.
bool Before(SortKey a, SortKey b)
{
	if (a.depth != b.depth)
		return a.depth > b.depth;
	if ((a.ordinal == 0) != (b.ordinal == 0))
		return a.ordinal == 0;
	if (a.group != b.group)
		return a.group < b.group;
	if (a.ordinal != b.ordinal)
		return a.ordinal < b.ordinal;
	if (a.object != b.object)
		return a.object < b.object;
	return a.first < b.first;
}

groupshared float sharedDepth[kTile];
groupshared uint sharedGroup[kTile];
groupshared uint sharedOrdinal[kTile];
groupshared uint sharedObject[kTile];
groupshared uint sharedFirst[kTile];

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID)
{
	ByteAddressBuffer counts = ResourceDescriptorHeap[CountIndex];
	const uint count = min(counts.Load((Options & kSortCountOffset) != 0 ? CountOffset : kCountForward), Capacity);
	if (groupID.x * kTile >= count)
		return;  // the whole group is past the count, uniform, so before any barrier
	RWByteAddressBuffer sequences = ResourceDescriptorHeap[SequencesIndex];
	const uint index = dispatchID.x;
	const bool staged = index < count;
	const SortKey mine = LoadKey(sequences, staged ? index : 0);

	uint rank = 0;
	[loop] for (uint tile = 0; tile < count; tile += kTile) {
		const uint other = tile + threadID.x;
		if (other < count) {
			const SortKey key = LoadKey(sequences, other);
			sharedDepth[threadID.x] = key.depth;
			sharedGroup[threadID.x] = key.group;
			sharedOrdinal[threadID.x] = key.ordinal;
			sharedObject[threadID.x] = key.object;
			sharedFirst[threadID.x] = key.first;
		}
		GroupMemoryBarrierWithGroupSync();
		const uint keys = min(kTile, count - tile);
		if (staged) {
			[loop] for (uint k = 0; k < keys; ++k) {
				if (tile + k == index)
					continue;
				SortKey key;
				key.depth = sharedDepth[k];
				key.group = sharedGroup[k];
				key.ordinal = sharedOrdinal[k];
				key.object = sharedObject[k];
				key.first = sharedFirst[k];
				rank += Before(key, mine) ? 1u : 0u;
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (!staged)
		return;

	const uint source = (StagingBase + index) * SequenceStride;
	const uint target = (StagingBase + Capacity + rank) * SequenceStride;
	const uint firstInstance = (Options & kSortRankInstance) != 0 ? rank : 0u;
	[loop] for (uint word = 0; word + 4 <= SequenceStride; word += 4) {
#if defined(__spirv__)
		const uint value = word == kInstanceCountOffset ? 1u : word == kVertexOffsetOffset ? 0u : word == kFirstInstanceOffset ? firstInstance : sequences.Load(source + word);
#else
		// On D3D12 the word before the draw's arguments carries the first instance too.
		const uint value = word == kInstanceCountOffset ? 1u : word == kVertexOffsetOffset ? 0u : (word == kFirstInstanceOffset || word == kInstanceCountOffset - 8) ? firstInstance : sequences.Load(source + word);
#endif
		sequences.Store(target + word, value);
	}
}

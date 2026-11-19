// Drawcall Limit Fix. The scatter step of the sort by pipeline. BuildDrawsCS staged each colour sequence with its rank under its
// sort key, and a prefix sum turned the per-key counts into first slots. Each sequence goes to its class's first slot plus its
// key's place plus its rank, and the staged words are restored. Each class's call then switches pipeline once per pipeline
// rather than per draw. Every slot is written exactly once.

// Must match SortSequencesConstants in Main/FrameLayout.h.
cbuffer SortSequencesConstants : register(b0)
{
	uint StagingIndex;    // ByteAddressBuffer: DrawSequence[DrawLimit], as BuildDrawsCS appended them
	uint RanksIndex;      // ByteAddressBuffer, each sequence's sort key << 20 | its rank
	uint OffsetsIndex;    // ByteAddressBuffer: uint[kSortKeys], each key's first slot
	uint CountIndex;      // ByteAddressBuffer, the draw count words
	uint SequencesIndex;  // RWByteAddressBuffer: DrawSequence[], what the draw executes
	uint SequenceStride;  // bytes, BuildDrawsCS's kSequenceStride
	uint DrawLimit;       // BuildDrawsCS's PhaseTwoBase
	uint Padding;
}

static const uint kCountDrawn = 0;
static const uint kSortRankBits = 20;  // BuildDrawsCS's
static const uint kSortRankMask = (1u << kSortRankBits) - 1;
static const uint kInstanceCountOffset = 120;  // the staged class's first slot
static const uint kFirstInstanceOffset = 132;  // the staged class's first sort key

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	ByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];
	const uint index = dispatchID.x;
	if (index >= min(count.Load(kCountDrawn), DrawLimit))
		return;
	ByteAddressBuffer staging = ResourceDescriptorHeap[StagingIndex];
	ByteAddressBuffer ranks = ResourceDescriptorHeap[RanksIndex];
	ByteAddressBuffer offsets = ResourceDescriptorHeap[OffsetsIndex];
	RWByteAddressBuffer sequences = ResourceDescriptorHeap[SequencesIndex];

	const uint source = index * SequenceStride;
	const uint4 head = staging.Load4(source);  // the pipeline, then the root constants
	const uint keyRank = ranks.Load(index * 4);
	const uint classFirst = staging.Load(source + kInstanceCountOffset);
	const uint classKeys = offsets.Load(staging.Load(source + kFirstInstanceOffset) * 4);
	const uint target = (classFirst + offsets.Load((keyRank >> kSortRankBits) * 4) - classKeys + (keyRank & kSortRankMask)) * SequenceStride;
	sequences.Store4(target, head);
	[loop] for (uint word = 16; word + 16 <= SequenceStride; word += 16)
		sequences.Store4(target + word, staging.Load4(source + word));
	[loop] for (uint tail = SequenceStride & ~15u; tail < SequenceStride; tail += 4)
		sequences.Store(target + tail, staging.Load(source + tail));
	sequences.Store(target + kInstanceCountOffset, 1u);
	sequences.Store(target + kFirstInstanceOffset, 0u);
}

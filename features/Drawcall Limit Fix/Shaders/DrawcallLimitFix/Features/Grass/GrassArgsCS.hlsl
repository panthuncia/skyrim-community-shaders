// Drawcall Limit Fix. Grass draw arguments. One thread per draw slot, bucket * 3 + tier, packs the slot's DrawInstanced into one
// of the draws buffer's arrays, counted for ExecuteIndirect. Empty slots pack nothing.
// Mode 1 packs phase 1's depth draws, mode 2 phase 2's, and mode 3 the colour draws, then zeroes the counts for the next frame.
// firstInstance is the start of the draw's visible range. D3D12's SV_InstanceID does not count from it, so each draw's arguments
// follow a word with its first instance, which D3D12's signature writes into the push data.

// Must match GrassArgsConstants in GrassTables.h.
cbuffer GrassArgsConstants : register(b0)
{
	uint DrawIndex;     // ByteAddressBuffer, GrassInstances::DrawRow, whose bucketSlots bounds the slots
	uint BucketsIndex;  // ByteAddressBuffer: GrassInstances::BucketRow per bucket slot
	uint MeshesIndex;   // ByteAddressBuffer: GrassInstances::MeshRow per mesh slot
	uint DrawsIndex;    // RWByteAddressBuffer: the draws buffer
	uint Mode;          // 1, 2 or 3
	uint DrawSlots;     // the slots the dispatch covers, buckets times 3
	uint Padding0;
	uint Padding1;
}

// GrassLayout.h.
static const uint kTiers = 3;
static const uint kMaxDrawSlots = 3 * 1024;
static const uint kBucketRowBytes = 224;
static const uint kMeshRowBytes = 32;
static const uint kNoMesh = 0xFFFFFFFFu;
static const uint kBucketTruePbr = 1;
static const uint kRowBucketSlots = 152;
// The draws buffer.
static const uint kPhaseOneDraws = 0;
static const uint kPhaseTwoDraws = 4;
static const uint kColourDraws = 8;
static const uint kCountsOffset = 64;
static const uint kPhaseOneCountsOffset = kCountsOffset + kMaxDrawSlots * 4;
static const uint kPhaseOneArgs = kPhaseOneCountsOffset + kMaxDrawSlots * 4;
static const uint kDrawArgsBytes = 20;  // the first instance word, then the arguments
static const uint kPhaseTwoArgs = kPhaseOneArgs + kMaxDrawSlots * kDrawArgsBytes;
static const uint kColourArgs = kPhaseTwoArgs + kMaxDrawSlots * kDrawArgsBytes;  // the second colour program's follow, kMaxDrawSlots * 20 on

void Pack(RWByteAddressBuffer a_draws, uint a_counter, uint a_array, uint a_vertices, uint a_instances, uint a_first)
{
	uint at;
	a_draws.InterlockedAdd(a_counter, 1u, at);
	const uint record = a_array + at * kDrawArgsBytes;
	a_draws.Store(record, a_first);
	a_draws.Store4(record + 4, uint4(a_vertices, a_instances, 0, a_first));
}

[numthreads(64, 1, 1)] void main(uint a_id : SV_DispatchThreadID)
{
	ByteAddressBuffer draw = ResourceDescriptorHeap[DrawIndex];
	RWByteAddressBuffer draws = ResourceDescriptorHeap[DrawsIndex];
	if (a_id >= DrawSlots || a_id >= kMaxDrawSlots)
		return;
	const uint bucket = a_id / kTiers;
	const uint tier = a_id % kTiers;
	const uint count = draws.Load(kCountsOffset + a_id * 4);
	if (Mode == 3) {
		// Zeroed whatever the slot draws, so a slot the next frame no longer names keeps no stale count.
		draws.Store(kCountsOffset + a_id * 4, 0u);
		draws.Store(kPhaseOneCountsOffset + a_id * 4, 0u);
	}
	if (bucket >= draw.Load(kRowBucketSlots))
		return;
	ByteAddressBuffer buckets = ResourceDescriptorHeap[BucketsIndex];
	const uint bucketBase = bucket * kBucketRowBytes;
	const uint mesh = buckets.Load(bucketBase + tier * 4);
	if (min(buckets.Load(bucketBase + 28), mesh ^ kNoMesh) == 0u)
		return;
	ByteAddressBuffer meshes = ResourceDescriptorHeap[MeshesIndex];
	const uint vertices = meshes.Load(mesh * kMeshRowBytes + 24);
	// Tiers 0 and 1 fill up from their base, tier 2 down from its own, as GrassCullCS's Append does.
	const uint base = buckets.Load(bucketBase + 16 + tier * 4);
	const uint first = tier == 2 ? base - count : base;
	if (Mode == 1) {
		draws.Store(kPhaseOneCountsOffset + a_id * 4, count);
		if (count)
			Pack(draws, kPhaseOneDraws, kPhaseOneArgs, vertices, count, first);
	} else if (Mode == 2) {
		const uint phaseOne = draws.Load(kPhaseOneCountsOffset + a_id * 4);
		if (count > phaseOne)
			Pack(draws, kPhaseTwoDraws, kPhaseTwoArgs, vertices, count - phaseOne, tier == 2 ? first : first + phaseOne);
	} else if (count) {
		const uint program = (buckets.Load(bucketBase + 12) & kBucketTruePbr) != 0 ? 1u : 0u;
		Pack(draws, kColourDraws + program * 4, kColourArgs + program * kMaxDrawSlots * kDrawArgsBytes, vertices, count, first);
	}
}

// Drawcall Limit Fix. The main camera's visibility as change events.
//
// The engine's cull leaves kAccumulated, NiAVObject flag bit 26, on roots and geometries it found in view. Animation updates,
// dynamic light receivers and the tree clock read it, so it must still be written once the engine's culls stop.
// DCLF has both verdicts on the GPU. A fade root's is FadeStateCS's kFadeVerdictInView, and a geometry's is depth phase 1's
// frustum stamp. HZB occlusion is not part of it, since the engine's cull does not make that test.
// This pass compares them once a frame with what it last reported per slot and appends only the changes. The host reads the
// list back asynchronously and decodes it on DCLF's executor.
// A root's word is keyed by its listing's generation. An object slot has no generation, so the host resolves its occupant with
// the tables of the event's frame. A change that does not fit stays unreported and is appended next frame, so none is dropped.

// Must match VisibilityEventsConstants in GpuLayouts.h.
cbuffer VisibilityEventsConstants : register(b0)
{
	uint RootsIndex;           // StructuredBuffer<FadeRootStatic>, the listings' generations
	uint StatesIndex;          // StructuredBuffer<FadeNodeState>, FadeStateCS's rows after this frame's update
	uint FrameIndex;           // StructuredBuffer<FadeFrame>, one row
	uint FrustumIndex;         // ByteAddressBuffer, per object depth phase 1's frustum stamp
	uint RootReportedIndex;    // RWStructuredBuffer<uint2>, per root the generation and bits last reported
	uint ObjectReportedIndex;  // RWStructuredBuffer<uint>, per object the bits last reported
	uint EventsIndex;          // RWStructuredBuffer<uint>, the header then the events
	uint EventCapacity;        // the events the list and every readback slot hold
	uint RootSlots;            // root threads, followed by the object threads
	uint ObjectSlots;          // object threads, the scene's object capacity
	uint Padding0;
	uint Padding1;
}

// Keep these the same as FadeStateCS.hlsl's and Records.h's.
struct FadeNodeState
{
	uint Flags;
	float CurrentFade;
	float SnapRadius;
	int LastVisible;
	float AmountFade;
	float Metric;
	float PreviousMetric;
	float Blend;
	uint Levels;
	uint Generation;
	uint Verdict;
	uint Frame;
};

struct FadeRootStatic
{
	FadeNodeState Initial;
	float Radius;
	float FadeAmount;
	float NearDistance;
	float FarDistance;
	uint Object;
	uint Bits;
	float LodScale;
	uint Generation;
};

struct FadeFrame
{
	float3 Eye;
	float LodAdjust;
	int Counter;
	float DeltaTime;
	uint FadesOn;
	uint LodUpdates;
	float FadeInTime;
	float FadeOutTime;
	float FadeInAbove;
	float FadeOutBelow;
	float BlendTime;
	float DistanceMult;
	float StepMax;
	float AmountTime;
	float MetricScale;
	float DefaultScale;
	float MetricOverride;
	uint Overridden;
	float LodFar;
	float LodNear;
	float TreeLodFar;
	float TreeLodNear;
	float LodMinimum;
	float One;
	float AmountSnapAbove;
	float AmountSnapOffset;
	float SnapRadiusLimit;
	float TreeHeightBase;
	float TreeHeightLimit;
	uint Padding;
	float4 Divisors[4];
	uint RootCount;
	uint SceneFrame;
	uint OutSlot;
	uint Reserved1;
	float3 AnimEye;
	float AnimLodAdjust;
	int AnimCounter;
	float AnimDeltaTime;
	uint2 AnimPadding;
};

static const uint kNoObject = 0xFFFFFFFFu;
static const uint kFadeVerdictInView = 1u << 0;
// The stamp in the low 28 bits, the fade drop in bit 31.
static const uint kFrustumStampMask = 0x0FFFFFFFu;
static const uint kFrustumFadeHidden = 0x80000000u;
// A four-word header with the appended count, which may pass capacity, and the run's stamp, which the host writes. Then four words per event.
static const uint kHeaderWords = 4;
static const uint kKindRoot = 0;
static const uint kKindObject = 1;
static const uint kBitVisible = 1u << 0;   // root InView, or an object stamped and not fade-hidden
static const uint kBitInMargin = 1u << 1;  // reserved for widened planes, not produced yet
static const uint kBitVerdict = 1u << 2;   // FadeStateCS updated the root's listing this frame
static const uint kBitFadeHidden = 1u << 3;  // an object stamped in view but dropped by its root's fade
// An object's reported word, with kReportedValid once a verdict was reported for the slot.
static const uint kReportedValid = 1u << 31;

static uint SceneFrame;

// Appends an event. False when it does not fit, so the caller keeps its word and it is appended next frame.
bool Append(uint a_slot, uint a_generation, uint a_kind, uint a_bits)
{
	RWStructuredBuffer<uint> events = ResourceDescriptorHeap[EventsIndex];
	uint at;
	InterlockedAdd(events[0], 1u, at);
	if (at >= EventCapacity)
		return false;
	const uint word = kHeaderWords + at * 4u;
	events[word] = a_slot;
	events[word + 1u] = a_generation;
	events[word + 2u] = a_kind;
	events[word + 3u] = a_bits;
	return true;
}

// A root slot's InView this frame against what was reported for its listing.
void RootSlot(uint a_slot, FadeFrame a_frame)
{
	// No camera this frame means FadeStateCS updated nothing, so the words stand.
	const uint rootCount = a_frame.LodAdjust != 0.0f ? a_frame.RootCount : 0u;
	if (a_slot >= rootCount)
		return;
	StructuredBuffer<FadeRootStatic> roots = ResourceDescriptorHeap[RootsIndex];
	const FadeRootStatic root = roots[a_slot];
	if (min(root.Object ^ kNoObject, root.Generation) == 0u)
		return;
	StructuredBuffer<FadeNodeState> states = ResourceDescriptorHeap[StatesIndex];
	const FadeNodeState state = states[a_slot];
	// A listing not updated this frame has no verdict.
	const bool updated = state.Generation == root.Generation && state.Frame == SceneFrame;
	const uint bits = updated ? (kBitVerdict | ((state.Verdict & kFadeVerdictInView) != 0 ? kBitVisible : 0u)) : 0u;
	RWStructuredBuffer<uint2> reported = ResourceDescriptorHeap[RootReportedIndex];
	const uint2 last = reported[a_slot];
	// A listing with no report yet has no verdict as far as the host knows.
	const uint lastBits = last.x == root.Generation ? last.y : 0u;
	if (lastBits == bits) {
		reported[a_slot] = uint2(root.Generation, bits);
		return;
	}
	if (Append(a_slot, root.Generation, kKindRoot, bits))
		reported[a_slot] = uint2(root.Generation, bits);
}

// An object slot's verdict against what was reported for the slot.
void ObjectSlot(uint a_slot)
{
	ByteAddressBuffer frustum = ResourceDescriptorHeap[FrustumIndex];
	RWStructuredBuffer<uint> reported = ResourceDescriptorHeap[ObjectReportedIndex];
	uint reportedSlots, stride;
	reported.GetDimensions(reportedSlots, stride);
	// ObjectSlots bounds the stamps. A raw GetDimensions would read a structured view's element count on D3D12.
	if (a_slot >= reportedSlots)
		return;
	const uint stamp = frustum.Load(a_slot * 4u);
	const bool stamped = (stamp & kFrustumStampMask) == (SceneFrame & kFrustumStampMask);
	const uint bits = stamped ? ((stamp & kFrustumFadeHidden) == 0 ? kBitVisible : kBitFadeHidden) : 0u;
	const uint last = reported[a_slot];
	// A slot with no report yet was out of view as far as the host knows.
	const uint lastBits = (last & kReportedValid) != 0 ? (last & ~kReportedValid) : 0u;
	if (lastBits == bits) {
		reported[a_slot] = kReportedValid | bits;
		return;
	}
	if (Append(a_slot, 0u, kKindObject, bits))
		reported[a_slot] = kReportedValid | bits;
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	StructuredBuffer<FadeFrame> frames = ResourceDescriptorHeap[FrameIndex];
	const FadeFrame frame = frames[0];
	SceneFrame = frame.SceneFrame;
	const uint index = dispatchID.x;
	if (index < RootSlots)
		RootSlot(index, frame);
	else if (index - RootSlots < ObjectSlots)
		ObjectSlot(index - RootSlots);
}

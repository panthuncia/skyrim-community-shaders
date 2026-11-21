// Drawcall Limit Fix: culls this frame's draw inputs and writes their indirect draw sequences (DrawSequence, Records.h) from the
// inputs and the geometry table. Compiled to SPIR-V at runtime (ComputeProgram) with BasicRHI's descriptor-heap ABI.

#include "Common/DeviceRef.hlsli"

#include "DrawcallLimitFix/CullEye.hlsli"
#include "ORGModuleServices/HzbTest.hlsli"
#include "DrawcallLimitFix/ParabolicPrograms.hlsli"
#include "DrawcallLimitFix/PortalPrograms.hlsli"
#include "DrawcallLimitFix/PortalQueries.hlsli"

// Push constants hold only what is fixed for a pass across executions. Per-execution values come from the latch block, written
// just before submission, so recorded commands can be reused. Must match BuildDrawsConstants and BuildDrawsLatch in Core/GpuLayouts.h.
cbuffer BuildDrawsConstants : register(b0)
{
	uint LatchIndex;          // ByteAddressBuffer: the latch block
	uint PushSequencesIndex;  // RWByteAddressBuffer: DrawSequence[]. ShadowViews: the views' table (StructuredBuffer<ShadowBuildView>)
	uint PushCountIndex;      // RWByteAddressBuffer: uint (zeroed before the dispatch). ShadowViews: the views' table's rows
	// Row strides for an input's y. Material row in the low 20 bits, pipeline row in the high 12, stride 0 for shadow views.
	uint MaterialRowStride;
	// Bits 4-7: the pass's culling phase; the mode and other bits come from the latch.
	uint PhaseBits;
	// One uint per object, written by the depth phases and read by the colour segment, so colour draws exactly what depth drew.
	uint VisibilityIndex;
	uint PushLatchOffset;  // this dispatch's BuildDrawsLatch, in bytes into the latch block. ShadowViews: the frame slot's region
	// Per object, the stamp of the last frame its bound was in the main frustum. Depth phase 1 only, decal, forward and refraction
	// inputs included. Feeds VisibilityEventsCS.
	uint FrustumIndex;
	// The HZB built after the depth pass, 0 when there is none, including the first frame.
	uint HzbIndex;
	uint HzbSizePacked;  // mip 0: width in the low 16 bits, height in the high 16
	uint HzbMips;
	// With SortCountsIndex set, a sequence goes to staging with its rank among its sort key's sequences, and OMS's SortScatter.hlsl scatters
	// it into its class's sub-range. 0 appends directly.
	uint SortCountsIndex;   // RWByteAddressBuffer: uint[kSortKeys], zero before the dispatch
	uint SortStagingIndex;  // RWByteAddressBuffer: DrawSequence[PhaseTwoBase]
	uint SortRanksIndex;    // RWByteAddressBuffer: uint[PhaseTwoBase]
	// Sequence ranges, in sequences. Phase 2 has its own fixed range since its draw is recorded on the CPU, which cannot know phase 1's
	// count. Decal sub-ranges follow, DecalStride slots each, slot = the decal's ordinal in the engine's draw order.
	uint PushPhaseTwoBase;  // ShadowCasterRows: the caster rows' capacity instead
	uint DecalBase;
	uint DecalStride;
	uint PipelineRowStride;
	// FadeStateCS's root model, 0 where unbound. Phase 1 drops an input while its root's OnVisible stops, a shadow view drops a caster
	// while its root fades, and every dispatch picks LOD partitions by the root's level.
	uint FadeRootsIndex;
	// Shadow views and colour. Each object's strict-light gate, which the latch's StrictGatesIndex verdicts refer to. 0 elsewhere.
	uint ObjectGatesIndex;
	// Shadow views and depth phases. A bucket count word per bucket. 0 elsewhere.
	uint PushBucketCountsIndex;
	// Shadow views and depth. Each geometry slot's first index in the index pool, ~0 without one.
	uint PoolFirstsIndex;
	// Main query preparation. Each root's program. Draw construction reads MemberResultsIndex instead.
	uint RootProgramsIndex;
	// Colour segment. The forward range. Visible forward draws append to their class's staging half with view depth in firstInstance.
	uint ForwardBase;
	uint ForwardCapacity;
	// Colour segment. The refraction range after the forward range's halves. Visible refraction draws append here, pooled.
	uint RefractionCapacity;
	// Colour segment. A count word per class and range. 0 elsewhere.
	uint ClassCountsIndex;
	uint MemberQueriesIndex;
	uint MemberResultsIndex;
	uint MemberClaimsIndex;
	// Depth phases. Phase 1 appends its OccludedRetest inputs here and grows phase 2's dispatch by a group per 64 entries.
	// Phase 2 runs over the list. 0 makes phase 2 run over every input.
	uint RetestsIndex;
	uint RetestDispatchIndex;
}

// The view being built. From the push constants, or under ShadowViews from each view's row in turn.
static uint SequencesIndex;
static uint CountIndex;
static uint LatchOffset;
static uint PhaseTwoBase;
static uint BucketCountsIndex;
// A shadow view's shared caster rows, and each object's first row in them. 0 elsewhere.
static uint CasterRowsIndex;
static uint CasterFirstsIndex;

// FadeStateCS's states as the previous frame published them.
static uint FadeStatesIndex;
// The frame's fade seeds, two rows per slot.
static uint FadeSeedsIndex;

// Must match Records.h's FadeNodeState and FadeRootStatic.
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
static const uint kFadeVerdictServiced = 1u << 2;
static const uint kFadeVerdictDrawn = 1u << 3;
static const uint kFadeFlagSettled = 1u << 15;

// A root's state until FadeStateCS first updates its generation. Its seed, else the row's.
FadeNodeState SeedState(uint a_root, FadeRootStatic a_row)
{
	if (FadeSeedsIndex != 0) {
		StructuredBuffer<FadeRootStatic> seeds = ResourceDescriptorHeap[FadeSeedsIndex];
		uint count, stride;
		seeds.GetDimensions(count, stride);
		const uint at = 2u * a_root + ((a_row.Bits & (1u << 23)) != 0u ? 1u : 0u);
		if (at < count && seeds[at].Generation == a_row.Generation)
			return seeds[at].Initial;
	}
	return a_row.Initial;
}

// Whether a fade root slot is not fully faded in. ~0u means no root.
bool RootFading(uint a_root)
{
	if (a_root == 0xFFFFFFFFu)
		return false;
	StructuredBuffer<FadeRootStatic> fadeRoots = ResourceDescriptorHeap[FadeRootsIndex];
	const FadeRootStatic row = fadeRoots[a_root];
	StructuredBuffer<FadeNodeState> fadeStates = ResourceDescriptorHeap[FadeStatesIndex];
	const FadeNodeState state = fadeStates[a_root];
	return (state.Generation == row.Generation ? state.CurrentFade : SeedState(a_root, row).CurrentFade) < 1.0;
}

// A LOD skin's partitions at its root's level (+0x152 & 0xF), as GetRenderPasses gives every view's pass.
// While the root crosses levels, a_copy gets the hint-10 copy, row L+1 less row L, drawn with the cross-fade factor as alpha.
// Main views only.
uint LodPartitions(uint a_partitions, uint a_lod, uint a_root, bool a_main, out uint a_copy)
{
	a_copy = 0;
	// One compare for every null guard (AGENTS.md shader rule). ~a_root is zero only for the no-root sentinel.
	if (min(min(a_lod, ~a_root), min(FadeRootsIndex, FadeStatesIndex)) == 0u)
		return a_partitions;
	StructuredBuffer<FadeRootStatic> fadeRoots = ResourceDescriptorHeap[FadeRootsIndex];
	const FadeRootStatic row = fadeRoots[a_root];
	StructuredBuffer<FadeNodeState> fadeStates = ResourceDescriptorHeap[FadeStatesIndex];
	FadeNodeState state = fadeStates[a_root];
	if (state.Generation != row.Generation)
		state = SeedState(a_root, row);
	const uint level = min(state.Levels & 0xFu, 3u);
	const uint mask = (a_lod >> (8u * level)) & 0xFFu;
	if (a_main && ((state.Levels >> 8) & 0x70u) != 0x20u && level < 3 && int(state.Blend * 31.0) > 0)
		a_copy = ((a_lod >> (8u * (level + 1))) & 0xFFu) & ~mask;
	return mask == 0 && a_copy == 0 ? (1u << 8) : mask;  // kNoPartitions, declared below
}

// Whether OnVisible stops at a fade root for a cull without cameraRelatedUpdates, such as the occlusion maps. An unsettled root
// recurses only while its fade is above 0 and its fadeAmount is not 0.
bool FadedOutOfOcclusion(uint a_root)
{
	if (a_root == 0xFFFFFFFFu)
		return false;
	StructuredBuffer<FadeRootStatic> fadeRoots = ResourceDescriptorHeap[FadeRootsIndex];
	const FadeRootStatic row = fadeRoots[a_root];
	StructuredBuffer<FadeNodeState> fadeStates = ResourceDescriptorHeap[FadeStatesIndex];
	FadeNodeState state = fadeStates[a_root];
	if (state.Generation != row.Generation)
		state = SeedState(a_root, row);
	if ((state.Flags & kFadeFlagSettled) != 0 && row.FadeAmount == 1.0 && state.CurrentFade == 1.0)
		return false;
	return !(state.CurrentFade > 0.0) || row.FadeAmount == 0.0;
}

// Per-execution values from the latch. Its first three words are this dispatch's indirect arguments.
static uint DrawCount;
// Bits 0-3 mode (0 off, 1 frustum, 2 frustum then HZB), 4-7 phase (push constants), bit 9 NoNearPlane.
static uint CullFlags;
// A word without the current stamp counts as visible to the colour segment. A needless draw fails its EQUAL test, while a stale
// "hidden" would drop an object that has depth and nothing else to shade it.
static uint VisibilityStamp;
// The HZB covers a larger power-of-two area, so this is the image-to-HZB UV ratio, 16-bit fixed point.
// Without it the test samples the far-plane padding and the HZB looks empty.
static uint HzbUvScalePacked;
// Camera-relative view-projection. World positions are made relative to the cull eye first.
static float4x4 ViewProj;
static float3 CullEye;
// Added to a bound's radius in tests on absolute positions, so rounding cannot cull.
static float CullMargin;
// A shadow view's caster volume in absolute world space, inside where dot(normal, p) - constant >= 0. 0 for the main camera.
static uint CullPlaneMask;
static float4 CullPlanes[6];
// A shadow view's pipeline map row, giving each key slot's pipeline under the view's rasterizer state. 0 for the main camera.
static uint PipelineMapOffset;
static uint SlotLayoutsOffset;  // a shadow view's key slot vertex layouts, 0 for none
// Main colour pass. kSunTestOn when the sun test applies, and the cascades' latch offset.
static uint SunState;
static const uint kSunTestOn = 0x80000000u;
static uint SunCascadeOffset;
// A sun view's full-frustum processes' latch offset, shared by the slot's sun views. 0 for no entry test.
static uint SunEntryOffset;
static const uint kSunRegionHeader = 16;
static const uint kSunCascadeBytes = 208;
static const uint kSunEntryProcessBytes = 112;
// Colour pass. Local shadow lights' latch offset. 0 for none.
static uint LocalShadowOffset;
static const uint kShadowVolumeBytes = 240;
// Depth segment: the main camera's position and LOD factor (kObjectFadeTest).
static float4 FadeEye;
// Depth segment: the tree height test's base and limit (kObjectHeightTest); limit +infinity when off.
static float2 TreeHeight;
// A shadow view's bucket table, giving each bucket its sequence range. 0 elsewhere.
static uint BucketTableOffset;
// Depth phases: each pipeline slot's bucket (latch bytes).
static uint BucketMapOffset;
// The view types this dispatch draws, 0 for every input.
static uint ViewBits;
// A focus view's target fade root slot + 1, which the input's root word must name. 0 for no test.
static uint FocusRoot;
// Depth segment while Terrain Blending holds the terrain. Its eye, and its near distance in w, 0 for no test.
static float4 TerrainNear;
// The colour segment's class table and the class map, in latch bytes. 0 for none.
static uint ClassTableOffset;
static uint ClassMapOffset;
// The frame's light context atlas and strict gates, and a portal-strict point light view's atlas entry + 1.
static uint LightContextsIndex;
static uint StrictGatesIndex;
static uint StrictContext;
// A sun view under kCullMinTexels. The least bound radius a caster keeps, in world units.
static float MinCasterRadius;

// The placement rows the culling reads.
struct PlacementRows
{
	float4 rows[10];
};
static const uint kObjectBoundRow = 6;
static const uint kObjectSunEntryRow = 7;
static const uint kObjectFadeNodeRow = 8;
static const uint kObjectForwardSortRow = 9;
// The frame's 160-byte placement rows. A move rewrites the row, never the inputs.
static uint PlacementsIndex;
// The installed publication's payload buffers.
static uint InputsBuffer;
static uint GeometriesBuffer;
static uint MaterialRowsLo;
static uint MaterialRowsHi;
static uint PipelineRowsLo;
static uint PipelineRowsHi;
// The installed material rows' buffer view ref, for reading their headers.
static uint2 MaterialRowsRef;
float4 ObjectRow(uint a_object, uint a_row)
{
	StructuredBuffer<PlacementRows> placements = ResourceDescriptorHeap[PlacementsIndex];
	return placements[a_object].rows[a_row];
}

void LoadViewLatch()
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint4 head = latch.Load4(LatchOffset + 12);  // after the dispatch arguments
	DrawCount = head.x;
	CullFlags = (head.y & ~0xF0u) | (PhaseBits & 0xF0u);
	VisibilityStamp = head.z;
	HzbUvScalePacked = head.w;
	CullPlaneMask = latch.Load(LatchOffset + 28);
	[unroll] for (uint p = 0; p < 6; ++p)
		CullPlanes[p] = asfloat(latch.Load4(LatchOffset + 96 + p * 16));
	ViewProj = float4x4(asfloat(latch.Load4(LatchOffset + 32)), asfloat(latch.Load4(LatchOffset + 48)),
		asfloat(latch.Load4(LatchOffset + 64)), asfloat(latch.Load4(LatchOffset + 80)));
	const float4 cullEye = LatchCullEye(latch, LatchOffset);
	CullEye = cullEye.xyz;
	CullMargin = cullEye.w;
	PipelineMapOffset = latch.Load(LatchOffset + 192);
	SlotLayoutsOffset = latch.Load(LatchOffset + 336);
	MinCasterRadius = asfloat(latch.Load(LatchOffset + 340));
	SunState = latch.Load(LatchOffset + 196);
	TreeHeight = asfloat(latch.Load2(LatchOffset + 200));
	FadeEye = asfloat(latch.Load4(LatchOffset + 208));
	const uint3 sunRegions = latch.Load3(LatchOffset + 224);
	SunCascadeOffset = sunRegions.x;
	SunEntryOffset = sunRegions.y;
	LocalShadowOffset = sunRegions.z;
	// Each depth phase has its own bucket table; they share the slot-to-bucket map.
	BucketTableOffset = latch.Load(LatchOffset + (((PhaseBits >> 4) & 0xFu) == 2 ? 244 : 236));
	BucketMapOffset = latch.Load(LatchOffset + 248);
	FadeStatesIndex = latch.Load(LatchOffset + 240);
	PlacementsIndex = latch.Load(LatchOffset + 252);
	FadeSeedsIndex = latch.Load(LatchOffset + 288);
	// The installed publication's payload, the scene list, the geometry table and the rows.
	const uint4 payload = latch.Load4(LatchOffset + 256);
	const uint4 rows = latch.Load4(LatchOffset + 272);
	InputsBuffer = payload.x;
	GeometriesBuffer = payload.y;
	ViewBits = payload.z;
	FocusRoot = payload.w;
	TerrainNear = asfloat(latch.Load4(LatchOffset + 304));
	const uint3 strict = latch.Load3(LatchOffset + 324);
	LightContextsIndex = strict.x;
	StrictGatesIndex = strict.y;
	StrictContext = strict.z;
	const uint2 classRegions = latch.Load2(LatchOffset + 292);
	ClassTableOffset = classRegions.x;
	ClassMapOffset = classRegions.y;
	MaterialRowsLo = rows.x;
	MaterialRowsHi = rows.y;
	PipelineRowsLo = rows.z;
	PipelineRowsHi = rows.w;
	MaterialRowsRef = latch.Load2(LatchOffset + 384);
}

// The dispatch's single view, from the push constants.
void LoadLatch()
{
	SequencesIndex = PushSequencesIndex;
	CountIndex = PushCountIndex;
	LatchOffset = PushLatchOffset;
	PhaseTwoBase = PushPhaseTwoBase;
	BucketCountsIndex = PushBucketCountsIndex;
	CasterRowsIndex = 0;
	CasterFirstsIndex = 0;
	LoadViewLatch();
}

// Light Limit Fix's shadow mask for an input, matching the main pass's selection. Lights whose sphere and planes contain the bound,
// less those that skip landscape, and portal-strict lights whose gate the object does not pass.
uint LocalShadowMask(float3 a_centre, float a_radius, bool a_landscape, uint a_object)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint volumes = latch.Load(LocalShadowOffset);
	uint mask = 0;
	[loop] for (uint v = 0; v < volumes; ++v)
	{
		const uint record = LocalShadowOffset + kSunRegionHeader + v * kShadowVolumeBytes;
		const uint4 head = latch.Load4(record);  // plane masks, mask bit, landscape
		const uint landscapeSkip = a_landscape && head.w == 0 ? 1u : 0u;
		if (((mask & head.z) | landscapeSkip) != 0u)
			continue;
		const float4 sphere = asfloat(latch.Load4(record + 16));
		const float3 offset = a_centre - sphere.xyz;
		const float reach = sphere.w + a_radius;
		if (dot(offset, offset) > reach * reach)
			continue;
		bool inside = true;
		[loop] for (uint p = 0; p < 12 && inside; ++p)
		{
			const uint planeMask = p < 6 ? head.x : head.y;
			if ((planeMask & (1u << (p % 6))) == 0)
				continue;
			const float4 plane = asfloat(latch.Load4(record + 32 + p * 16));
			inside = dot(plane.xyz, a_centre) - plane.w >= -a_radius;
		}
		const uint portalContext = latch.Load(record + 224);
		if (inside && portalContext != 0 && StrictGateRejects(StrictGatesIndex, LightContextsIndex, ObjectGatesIndex, portalContext - 1, a_object, a_centre, a_radius))
			inside = false;
		if (inside)
			mask |= head.z;
	}
	return mask;
}

// Whether the bound meets any cascade, using SunAccumulation's sphere test as the engine's cascade culls do.
bool InSunCascades(float3 a_centre, float a_radius)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint cascades = latch.Load(SunCascadeOffset);
	[loop] for (uint c = 0; c < cascades; ++c)
	{
		const uint record = SunCascadeOffset + kSunRegionHeader + c * kSunCascadeBytes;
		const uint2 masks = latch.Load2(record);
		bool inside = true;
		[loop] for (uint p = 0; p < 12 && inside; ++p)
		{
			const uint mask = p < 6 ? masks.x : masks.y;
			if ((mask & (1u << (p % 6))) == 0)
				continue;
			const float4 plane = asfloat(latch.Load4(record + 16 + p * 16));
			inside = dot(plane.xyz, a_centre) - plane.w >= -a_radius;
		}
		if (inside)
			return true;
	}
	return false;
}

// Whether an entry sphere is outside every full-frustum process. None means no verdict.
bool OutsideSunEntry(float4 a_entry)
{
	if (SunEntryOffset == 0)
		return false;
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint processes = latch.Load(SunEntryOffset);
	if (processes == 0)
		return false;
	[loop] for (uint process = 0; process < processes; ++process)
	{
		const uint record = SunEntryOffset + kSunRegionHeader + process * kSunEntryProcessBytes;
		const uint mask = latch.Load(record);
		bool outside = false;
		[loop] for (uint p = 0; p < 6 && !outside; ++p)
		{
			if ((mask & (1u << p)) == 0)
				continue;
			const float4 plane = asfloat(latch.Load4(record + 16 + p * 16));
			outside = dot(plane.xyz, a_entry.xyz) - plane.w < -(a_entry.w + CullMargin);
		}
		if (!outside)
			return false;
	}
	return true;
}

// kNoPipeline drops the draw rather than execute an index outside the set. The CPU never lets an input reach it.
static const uint kNoPipeline = 0xFFFFFFFFu;
uint DrawPipeline(uint a_input)
{
	if (PipelineMapOffset == 0)
		return a_input;
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	return latch.Load(PipelineMapOffset + a_input * 4);
}

uint CullMode() { return CullFlags & 0xF; }
// 1 is depth phase 1, against last frame's HZB. 2 is depth phase 2, against this frame's HZB over what phase 1 rejected.
// 3 is the colour segment, which only reads what the two phases decided.
uint CullPhase() { return (CullFlags >> 4) & 0xF; }
static const uint kPhaseSingle = 0;
static const uint kPhaseOne = 1;
static const uint kPhaseTwo = 2;
static const uint kPhaseColour = 3;

// Frustum rejection is final. A phase 1 occlusion rejection is provisional, since it uses last frame's depth.
static const uint kVisibilityOccludedRetest = 0;
static const uint kVisibilityVisible = 1;
static const uint kVisibilityRejectedFinal = 2;
// Written only by the colour segment, for an object it drew with no verdict published this frame.
static const uint kVisibilityUnpublished = 3;
// Verdict in bits 0-1, depth-drawn bit 2, colour-drawn bit 3, stamp above. Depth without colour, or the reverse, is visible damage.
static const uint kVisibilityVerdictMask = 3;
static const uint kVisibilityDepthDrawn = 4;
static const uint kVisibilityColourDrawn = 8;
static const uint kVisibilityStampShift = 4;
// A clamped shadow view pancakes casters in front of its near plane onto it, so keep them.
bool NoNearPlane() { return (CullFlags & 0x200) != 0; }
// The volumetric lighting copy's views draw only volumetric-only casters, every other shadow view the rest.
bool CastersOnly() { return (CullFlags & 0x400) != 0; }
bool VolumetricOnly() { return (CullFlags & 0x800) != 0; }
// A sun view: an input whose entry is outside the full-frustum processes is not a caster (kCullSunEntry).
bool SunEntry() { return (CullFlags & 0x1000) != 0; }
// Skylighting's occlusion map draws nothing with a bound radius of 32 or less.
bool MinRadius() { return (CullFlags & 0x2000) != 0; }
// The occlusion map tests fade roots as OnVisible without cameraRelatedUpdates.
bool FadeOnVisible() { return (CullFlags & 0x4000) != 0; }
// A sun view skips casters whose radius projects to fewer texels than the threshold.
bool MinTexels() { return (CullFlags & 0x8000) != 0; }
// A point light hemisphere is culled by the light's sphere and equatorial plane, as BSParabolicCullingProcess does, not by the
// view-projection. Front takes FUN_14151a1e0's codes 1 and 3, back takes 2 and 3.
bool Parabolic() { return (CullFlags & 0x80000) != 0; }
// Object word bit 30 marks a LOD cross-fade copy. The pixel stage uses the cross-fade factor as alpha.
static const uint kObjectCrossfadeCopy = 1u << 30;
// The key word a view reads. 0 for pipeline and rows, 1 for a caster's shadow key, 2 and 3 for an occlusion map's keys.
uint KeyWord() { return (CullFlags >> 16) & 3; }

// Object flags from Records.h. The object word also marks a draw that misses every cascade, for the pixel stage.
static const uint kObjectSunTest = 1u << 26;
static const uint kObjectSunMiss = 1u << 31;
// The object word's local shadow lights, and a landscape flag.
static const uint kObjectLocalShadowShift = 26;
static const uint kObjectLandscapeLights = 1u << 29;
static const uint kObjectMember = 1u << 30;
// Under a fade root with a fade-out distance, phase 1 drops it past that distance unless it was in view last frame.
static const uint kObjectFadeTest = 1u << 27;
// A resident tree, dropped by phase 1 where BSTreeNode::OnVisible's height test would.
static const uint kObjectHeightTest = 1u << 28;
// A volumetric-only caster, drawn only by the volumetric copy's views.
static const uint kObjectVolumetricOnly = 1u << 23;
// A decal, group in bits 20-21. Never an occluder or depth input. The colour segment tests it once against this frame's HZB and
// writes fixed slots, so overlapping decals draw in the engine's order.
static const uint kObjectDecal = 1u << 7;
// kObjectShadowOnly, and with kObjectDecal kObjectFocusOnly. No other shadow input carries kObjectDecal.
static const uint kObjectShadowOnlyBit = 1u << 24;
static const uint kObjectFocusOnly = kObjectShadowOnlyBit | kObjectDecal;
static const uint kObjectDecalGroupShift = 20;
// Without kObjectDecal the same bits are a terrain group. 1 is Terrain Blending's terrain, 2 the meshes it redraws after.
// The depth segment sends a group 1 draw within the near distance to its bucket's twin.
uint TerrainGroup(uint a_flags) { return (a_flags & kObjectDecal) != 0 ? 0 : (a_flags >> kObjectDecalGroupShift) & 3; }
static const uint kBucketTerrainPair = 0x80000000u;
// A blended Lighting object for the forward epoch. Like a decal, the colour segment culls it once and stages it with its view
// depth for ForwardSortCS.
static const uint kObjectForward = 0x80000000u;
// A refraction object, culled as a forward input plus its root's fade verdict. No sort, since the engine draws group 5 by batches
// and the draw neither blends nor writes depth.
static const uint kViewRefraction = 1u << 10;
// The first-person view frustum-culls a forward input and sends it to the forward range, staged with its sort key.
static const uint kViewFirstPerson = 1u << 11;
static const uint kBucketForward = 0x40000000u;
// Matches TerrainBlending.cpp's test exactly.
bool TerrainNearOf(float4 a_bound) { return distance(a_bound.xyz, TerrainNear.xyz) - a_bound.w <= TerrainNear.w; }
// This input may get a sequence. The depth segment submits every candidate so the published visibility is complete.
static const uint kInputDrawable = 1u << 16;

// Counter words in the count buffer, zeroed before the dispatch.
static const uint kCountDrawn = 0;   // sequences written
static const uint kCountCulled = 4;  // rejected by this frame's culling
static const uint kCountTested = 8;  // looked at by the culling at all
// What a shadow view drops before culling.
static const uint kCountSunEntryOut = 12;
static const uint kCountMinRadius = 16;
static const uint kCountRootFading = 20;
static const uint kCountCasterClass = 64;
static const uint kCountOccluded = 24;  // rejected by the HZB rather than by the frustum
// What the HZB held under tested objects, to tell a suspicious rejection count from an empty or full HZB.
static const uint kCountHzbNear = 28;     // farthest depth ~0, the build wrote nothing
static const uint kCountHzbFar = 32;      // ~1, all far plane, nothing can be occluded
static const uint kCountHzbSampled = 36;  // the denominator for the two above
// One rejection, recorded in full.
static const uint kSampleTaken = 40;
static const uint kSampleFarthest = 44;
static const uint kSampleNearestZ = 48;
static const uint kSampleUvMin = 52;
static const uint kSampleUvMax = 56;
static const uint kSampleMip = 60;
static const uint kCountDrawnPhaseTwo = 68;      // sequences phase 2 appended, and the count its draw reads
static const uint kCountRescuedByPhaseTwo = 72;  // objects phase 1 rejected and the rebuilt HZB brought back
// The decal groups' slot counts, uploaded by the CPU, and the culling's tallies.
static const uint kCountDecalGroup1 = 76;
static const uint kCountDecalGroup2 = 80;
static const uint kCountDecalsCulled = 84;
static const uint kCountDecalsTested = 88;
// Sun test inputs tested, and those that missed every cascade.
static const uint kCountSunTested = 92;
static const uint kCountSunMissed = 96;
// Fade test inputs found in the frustum, and those dropped.
static const uint kCountFadeTested = 100;
static const uint kCountFadeHidden = 104;
// Terrain group draws appended, then phase 1's terrain group 1 inputs and those found near.
static const uint kCountTerrainGroup1 = 112;
static const uint kCountTerrainNearTested = 120;
static const uint kCountTerrainNear = 124;
// Forward draws appended, then forward inputs tested, in the frustum, and HZB-rejected.
static const uint kCountForward = 128;
static const uint kCountForwardTested = 132;
static const uint kCountForwardInView = 136;
static const uint kCountForwardOccluded = 140;
// The refraction draw count, then refraction inputs tested, in the frustum, and rejected by HZB or fade.
static const uint kCountRefraction = 144;
static const uint kCountRefractionTested = 148;
static const uint kCountRefractionInView = 152;
static const uint kCountRefractionOccluded = 156;
// Colour draws dropped for want of a state class sub-range. Never drawn with another class's state.
static const uint kCountClassUnplanned = 160;
static const uint kCountClassOverflow = 164;
// Phase 1's retest list length, and a sun view's casters below the size threshold.
static const uint kCountRetests = 168;
static const uint kCountBelowTexels = 172;
// Words 176-207 are unused.
// Casters a shadow view dropped for want of a caster row.
static const uint kCountCasterRowsLost = 208;

// The stamp in the low 28 bits, and whether the fade test dropped the object that frame.
static const uint kFrustumStampMask = 0x0FFFFFFFu;
static const uint kFrustumFadeHidden = 0x80000000u;

// Sort keys, partitioned by the state classes. A rank word holds the key in its top 12 bits and the rank below.
static const uint kSortKeys = 4096;
static const uint kSortRankBits = 20;

// A class map word is class << kSetClassShift | the pipeline's index in its class's execution sets.
// A class table entry is the class's first sort key and size, then first and capacity per range.
static const uint kSetClassShift = 16;
static const uint kSetClassIndexMask = 0xFFFFu;
static const uint kNoSetClass = 0xFFFFFFFFu;
static const uint kClassRangeMain = 0;
static const uint kClassRangeForward = 6;
static const uint kClassRanges = 7;
static const uint kClassHeaderBytes = 32;  // GpuLayouts.h, kClassHeaderWords
static const uint kClassBytes = 64;  // kClassWords words

// A set index's state class and index from the class map. False without a word for it, and the draw is then dropped and counted.
bool SetClassOf(uint a_set, out uint a_class, out uint a_index)
{
	a_class = 0;
	a_index = 0;
	if (ClassMapOffset == 0)
		return false;
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	if (a_set >= latch.Load(ClassMapOffset))
		return false;
	const uint word = latch.Load(ClassMapOffset + kClassHeaderBytes + a_set * 4);
	if (word == kNoSetClass)
		return false;
	a_class = word >> kSetClassShift;
	a_index = word & kSetClassIndexMask;
	return true;
}

// The class count, 0 without a table.
uint ClassCount()
{
	if (ClassTableOffset == 0)
		return 0;
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	return latch.Load(ClassTableOffset);
}

// A class's first slot and capacity in a range, 0 when it has none there.
uint2 ClassRange(uint a_class, uint a_range)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	return latch.Load2(ClassTableOffset + kClassHeaderBytes + a_class * kClassBytes + 8 + a_range * 8);
}

// A class's first sort key, so each class's keys are contiguous and its draws sort within it.
uint ClassKeyBase(uint a_class)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	return latch.Load(ClassTableOffset + kClassHeaderBytes + a_class * kClassBytes);
}

// The next slot of a class's sub-range, or false past capacity. The count then exceeds capacity, which the call's max count
// clamps, so every executed slot is written.
bool TakeClassSlot(uint a_class, uint a_range, out uint a_slot)
{
	const uint2 range = ClassRange(a_class, a_range);
	RWByteAddressBuffer classCounts = ResourceDescriptorHeap[ClassCountsIndex];
	uint taken;
	classCounts.InterlockedAdd((a_class * kClassRanges + a_range) * 4, 1, taken);
	a_slot = range.x + taken;
	if (taken < range.y)
		return true;
	RWByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];
	uint scratch;
	count.InterlockedAdd(kCountClassOverflow, 1, scratch);
	return false;
}

// Plain draw buckets in the class map's tail. Returns how many a section has, 0 when the class sub-ranges take its draws, and a set
// index's bucket with its range, false for none, which drops and counts the draw.
static const uint kNoPlainBucket = 0xFFFFFFFFu;  // GpuLayouts.h, kNoBucket
static const uint kForwardSection = 1, kMainSection = 2, kTerrainSection = 3, kDecalSection = 4;  // their header words
uint PlainBuckets(uint a_section)
{
	if (ClassMapOffset == 0)
		return 0;
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	return latch.Load(ClassMapOffset + 4 * a_section);
}
bool PlainBucketOf(uint a_section, uint a_set, out uint a_bucket, out uint2 a_range, out uint a_countWord)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint sets = latch.Load(ClassMapOffset);
	// Skip the earlier sections and their buckets' count words.
	uint section = ClassMapOffset + kClassHeaderBytes + sets * 4;
	uint earlier = 0;
	[loop] for (uint s = kForwardSection; s < a_section; ++s) {
		const uint buckets = latch.Load(ClassMapOffset + 4 * s);
		if (buckets != 0)
			section += (sets + 2 * buckets) * 4;
		earlier += buckets;
	}
	a_bucket = latch.Load(section + a_set * 4);
	a_range = uint2(0, 0);
	a_countWord = ClassCount() * kClassRanges + earlier + a_bucket;
	if (a_bucket == kNoPlainBucket)
		return false;
	a_range = latch.Load2(section + (sets + 2 * a_bucket) * 4);
	return true;
}

// DrawInput from Core/GpuLayouts.h, 64 bytes.
static const uint kInputStride = 64;
// GeometryDraw from Core/GpuLayouts.h, 40 bytes.
static const uint kGeometryStride = 40;
// A multi-partition skin draws once per partition the engine would. The input's mask names them and each GeometryDraw links to the next.
static const uint kMaxPartitions = 8;
static const uint kNoPartition = 0xFFFFFFFFu;
// A skin whose LOD level draws none of its partitions, so no draw at all.
static const uint kNoPartitions = 1u << 8;
// An object LOD chain. Every slot is linked from the first, with the count in the low bits.
static const uint kPartitionChain = 1u << 15;
static const uint kPartitionChainCount = kPartitionChain - 1;
// DrawSequence from Records.h, 136 bytes, 4-byte packed. Words 1-24 are the push data, laid out as DrawPipelines.h's kDrawPush*.
// The pad keeps pushed addresses 8-byte aligned for BasicRHI. Slot 1's vertex view is a face shape's positions, else slot 0 again.
static const uint kSequenceStride = 136;
static const uint kSequenceDrawOffset = 116;
// The staged sort key words that ForwardSortCS and OMS's SortScatter.hlsl read.
static const uint kSequenceInstanceCount = 120;
static const uint kSequenceVertexOffset = 128;
static const uint kSequenceFirstInstance = 132;
// The rows' blocks, as DrawPipelines.h's kPipelineRow* and kMaterialRowPS.
static const uint kPipelineRowTechniquePS = 256;
static const uint kPipelineRowGeometryVS = 512;
static const uint kPipelineRowGeometryPS = 768;
static const uint kPipelineRowPermutation = 1280;
static const uint kMaterialRowPS = 256;
static const uint kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT
static const uint kNoStream = 0xFFFFFFFFu;

uint2 AddressOf(uint a_lo, uint a_hi, uint a_offset)
{
	const uint lo = a_lo + a_offset;
	return uint2(lo, a_hi + (lo < a_lo ? 1 : 0));
}

void StoreSequence(RWByteAddressBuffer sequences, uint slot, uint pipeline, uint4 rows, uint objectIndex, uint4 vertexBuffer,
	uint4 streamBuffer, uint4 indexBuffer, uint indexCount, uint firstIndex)
{
	const uint base = slot * kSequenceStride;
	sequences.Store(base + 0, pipeline);
	sequences.Store4(base + 4, rows);
	sequences.Store(base + 20, objectIndex);
	sequences.Store4(base + 24, vertexBuffer);
	sequences.Store4(base + 40, streamBuffer);
	// The rows' other blocks. A shadow view's layout maps only the material row and object word, so these are unread there.
	sequences.Store(base + 56, 0);  // the pad
	sequences.Store4(base + 60, uint4(AddressOf(rows.x, rows.y, kPipelineRowTechniquePS), AddressOf(rows.x, rows.y, kPipelineRowGeometryVS)));
	sequences.Store4(base + 76, uint4(AddressOf(rows.x, rows.y, kPipelineRowGeometryPS), AddressOf(rows.x, rows.y, kPipelineRowPermutation)));
	sequences.Store2(base + 92, AddressOf(rows.z, rows.w, kMaterialRowPS));
	sequences.Store4(base + 100, uint4(indexBuffer.xyz, kIndexFormatR16));
	sequences.Store4(base + kSequenceDrawOffset, uint4(indexCount, 1, firstIndex, 0));
	sequences.Store(base + kSequenceFirstInstance, 0);
}

// A shadow view's sequence is a plain indexed draw from the index pool, no vertex offset, and the bucket slot as first instance.
void StoreShadowSequence(RWByteAddressBuffer sequences, uint slot, uint instance, uint4 rows, uint objectIndex, uint4 vertexBuffer,
	uint4 streamBuffer, uint4 indexBuffer, uint indexCount, uint firstIndex, uint a_poolFirst)
{
	StoreSequence(sequences, slot, 0, rows, objectIndex, vertexBuffer, streamBuffer, indexBuffer, indexCount, firstIndex);
	const bool pooled = a_poolFirst != 0xFFFFFFFFu;
	sequences.Store4(slot * kSequenceStride + kSequenceDrawOffset, uint4(pooled ? indexCount : 0, 1, pooled ? a_poolFirst + firstIndex : 0, 0));
	sequences.Store(slot * kSequenceStride + kSequenceFirstInstance, instance);
#if !defined(__spirv__)
	// D3D12's SV_InstanceID does not count the first instance, so the plain draws' command signature also writes this word into the push data.
	sequences.Store(slot * kSequenceStride + kSequenceDrawOffset - 4, instance);
#endif
}

// ---- Shadow view draws. Arguments go in the view's own buffer, and everything about the object goes in one shared caster row,
// laid out as the pulled vertex stage reads a DrawSequence. A row is read only by a draw this frame's culling wrote. ----
// D3D12's signature reads the first word into the draw push word, since SV_InstanceID does not count the first instance there.
static const uint kShadowDrawStride = 24;

void StoreCasterRow(RWByteAddressBuffer a_rows, uint a_row, uint a_technique, uint4 a_rowWords, uint a_objectWord, uint4 a_vertexBuffer, uint4 a_streamBuffer)
{
	const uint base = a_row * kSequenceStride;
	a_rows.Store(base + 0, a_technique);
	a_rows.Store4(base + 4, a_rowWords);
	a_rows.Store(base + 20, a_objectWord);
	a_rows.Store4(base + 24, a_vertexBuffer);
	a_rows.Store4(base + 40, a_streamBuffer);
}

void StoreShadowDraw(RWByteAddressBuffer a_draws, uint a_slot, uint a_row, uint a_indexCount, uint a_firstIndex, uint a_poolFirst)
{
	const bool pooled = a_poolFirst != 0xFFFFFFFFu;
	const uint base = a_slot * kShadowDrawStride;
	a_draws.Store(base, a_row);
	a_draws.Store4(base + 4, uint4(pooled ? a_indexCount : 0, 1, pooled ? a_poolFirst + a_firstIndex : 0, 0));
	a_draws.Store(base + 20, a_row);
}

// The partitions an input can draw in any frame, its LOD levels' masks ORed.
uint CasterPartitionMask(uint a_partitions, uint a_lod)
{
	if ((a_partitions & kPartitionChain) != 0)
		return a_partitions;
	return a_partitions | (a_lod != 0 ? ((a_lod | (a_lod >> 8) | (a_lod >> 16) | (a_lod >> 24)) & 0xFFu) : 0u);
}

// An input's rows in one view family, one per partition.
uint CasterSpan(uint a_mask)
{
	if ((a_mask & kPartitionChain) != 0)
		return a_mask & kPartitionChainCount;
	return a_mask == 0 ? 1u : countbits(a_mask & (kNoPartitions - 1));
}

uint CasterOrdinal(uint a_mask, uint a_partition)
{
	const uint counted = (a_mask & kPartitionChain) != 0 ? 0u : a_mask;
	if (counted == 0u)
		return a_partition;
	return countbits(counted & ((1u << a_partition) - 1u));
}

// ---- The one place a draw's material row address is computed. ----
uint2 MaterialRowAddress(uint a_materialSlot)
{
	const uint2 installed = AddressOf(MaterialRowsLo, MaterialRowsHi, a_materialSlot * MaterialRowStride);
	// A row a frame patch writes is drawn from the frame's patched copy, which TextureTransformsCS made before this frame's draws.
	// 0 means no copy this frame, so use the installed row.
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint2 copyRows = latch.Load2(LatchOffset + 348);
	if ((copyRows.x | copyRows.y) == 0)
		return installed;
	const uint animatedRow = DeviceLoad(uint, DeviceRefAdd(MaterialRowsRef, a_materialSlot * MaterialRowStride + 768), 4);
	if (animatedRow == 0xFFFFFFFFu)
		return installed;
	return AddressOf(copyRows.x, copyRows.y, animatedRow * 1024);  // kMaterialRowBytes
}

// An input's packed rows as addresses, the draw's first four push words.
uint4 RowsOf(uint a_rows)
{
	const uint2 pipelineRow = PipelineRowStride ? AddressOf(PipelineRowsLo, PipelineRowsHi, (a_rows >> 16) * PipelineRowStride) : uint2(0, 0);
	const uint2 materialRow = MaterialRowAddress(a_rows & 0xFFFFu);
	return uint4(pipelineRow, materialRow);
}

// A staged forward draw's sort key, which ForwardSortCS reads and restores. View depth goes in firstInstance. A BSOrderedNode
// group's leaf sorts at its node's centre, where the engine places the whole group, with node hash and ordinal in the other words.
void StageForwardKey(RWByteAddressBuffer sequences, uint a_slot, uint a_object)
{
	const float4 forwardSort = ObjectRow(a_object, kObjectForwardSortRow);
	const uint ordinal = asuint(forwardSort.w);
	const float3 sortCentre = ordinal != 0 ? forwardSort.xyz : ObjectRow(a_object, kObjectBoundRow).xyz;
	const precise float3 sortRelative = sortCentre - CullEye;
	const float clipW = mul(ViewProj, float4(sortRelative, 1.0)).w;
	const uint group = ordinal != 0 ? (asuint(forwardSort.x) * 0x9E3779B1u) ^ (asuint(forwardSort.y) * 0x85EBCA77u) ^ (asuint(forwardSort.z) * 0xC2B2AE3Du) : 0u;
	const uint stagedAt = a_slot * kSequenceStride;
	sequences.Store(stagedAt + kSequenceInstanceCount, group);
	sequences.Store(stagedAt + kSequenceVertexOffset, ordinal);
	sequences.Store(stagedAt + kSequenceFirstInstance, asuint(isnan(clipW) ? 0.0 : clipW));  // a NaN would break the rank
}

// Projects the bound's world AABB corner by corner, rejecting only when all eight are outside one clip plane. Tests are on
// homogeneous coordinates, so corners behind the eye still work and an object wholly behind the camera fails the near plane.
bool Culled(float3 boundCentre, float boundRadius)
{
	// The caster volume's planes are absolute, so the margin keeps rounding from culling.
	[unroll] for (uint p = 0; p < 6; ++p)
	{
		if ((CullPlaneMask & (1u << p)) && dot(CullPlanes[p].xyz, boundCentre) - CullPlanes[p].w < -(boundRadius + CullMargin))
			return true;
	}
	// Corners project eye-relative, so large terms never cancel in float.
	const precise float3 centre = boundCentre - CullEye;
	float4 planes = float4(1, 1, 1, 1);  // all-corners-outside accumulators for -x, +x, -y, +y
	float2 depthPlanes = float2(1, 1);   // near, far
	[unroll] for (uint corner = 0; corner < 8; ++corner)
	{
		const float3 offset = float3((corner & 1) ? boundRadius : -boundRadius,
			(corner & 2) ? boundRadius : -boundRadius,
			(corner & 4) ? boundRadius : -boundRadius);
		const float4 clip = mul(ViewProj, float4(centre + offset, 1.0));
		planes.x *= (clip.x < -clip.w) ? 1 : 0;
		planes.y *= (clip.x > clip.w) ? 1 : 0;
		planes.z *= (clip.y < -clip.w) ? 1 : 0;
		planes.w *= (clip.y > clip.w) ? 1 : 0;
		depthPlanes.x *= (clip.z < 0 && !NoNearPlane()) ? 1 : 0;
		depthPlanes.y *= (clip.z > clip.w) ? 1 : 0;
	}
	return any(planes != 0) || any(depthPlanes != 0);
}

bool ParabolicRejected(float3 a_centre, float a_radius)
{
	// Absolute, so the margin grows the sphere. The plane moves only for the back hemisphere, whose test a bound near the plane can fail.
	const bool back = (CullFlags & 0x100000) != 0;
	const float4 sphere = float4(CullPlanes[0].xyz, CullPlanes[0].w + CullMargin);
	const float4 plane = float4(CullPlanes[1].xyz, CullPlanes[1].w + (back ? CullMargin : 0.0));
	const uint code = ParabolicSphereMask(float4(a_centre, a_radius), sphere, plane, (CullFlags & 0x200000) != 0);
	return back ? (code & 2u) == 0 : (code & 1u) == 0;
}

// Occlusion against the HZB, with diagnostic counters.
bool Occluded(float3 boundCentre, float boundRadius)
{
	HzbView view;
	view.ViewProj = ViewProj;
	view.Eye = CullEye;
	view.Index = HzbIndex;
	view.BaseSize = HzbUnpackSize(HzbSizePacked);
	view.Mips = HzbMips;
	view.UvScale = HzbUnpackUvScale(HzbUvScalePacked);
	const HzbResult result = HzbTest(view, boundCentre, boundRadius);
	if (!result.Sampled)
		return false;

	RWByteAddressBuffer counters = ResourceDescriptorHeap[CountIndex];
	uint scratch;
	counters.InterlockedAdd(kCountHzbSampled, 1, scratch);
	if (result.Farthest <= 1e-6)
		counters.InterlockedAdd(kCountHzbNear, 1, scratch);
	else if (result.Farthest >= 0.9999)
		counters.InterlockedAdd(kCountHzbFar, 1, scratch);

	// The dispatch's first rejection in full, so a suspicious count can be read.
	if (result.Occluded) {
		uint previous;
		counters.InterlockedCompareExchange(kSampleTaken, 0, 1, previous);
		if (previous == 0) {
			counters.Store(kSampleFarthest, asuint(result.Farthest));
			counters.Store(kSampleNearestZ, asuint(result.NearestZ));
			counters.Store(kSampleUvMin, (uint(saturate(result.UvMin.x) * 65535.0) & 0xFFFF) | (uint(saturate(result.UvMin.y) * 65535.0) << 16));
			counters.Store(kSampleUvMax, (uint(saturate(result.UvMax.x) * 65535.0) & 0xFFFF) | (uint(saturate(result.UvMax.y) * 65535.0) << 16));
			counters.Store(kSampleMip, uint(result.Mip));
		}
	}
	return result.Occluded;
}

// Stamps the feedback word of a phase 1 input that passed the frustum and portal test, and returns whether its root's fade dropped it.
bool StampFrustum(ByteAddressBuffer a_inputs, uint a_inputOffset, uint a_flags, uint a_objectIndex, bool a_count)
{
	RWByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];
	uint scratch;
	bool fadeHidden = false;
	RWByteAddressBuffer frustumStamps = ResourceDescriptorHeap[FrustumIndex];
	// Under a fade root, use FadeStateCS's previous OnVisible verdict, as the engine's cull decides whether to recurse.
	const uint fadeRoot = a_inputs.Load(a_inputOffset + 48);
	bool serviced = false;
	if (fadeRoot != 0xFFFFFFFFu && FadeRootsIndex != 0) {
		StructuredBuffer<FadeRootStatic> fadeRoots = ResourceDescriptorHeap[FadeRootsIndex];
		const FadeRootStatic rootRow = fadeRoots[fadeRoot];
		StructuredBuffer<FadeNodeState> fadeStates = ResourceDescriptorHeap[FadeStatesIndex];
		const FadeNodeState fadeState = fadeStates[fadeRoot];
		serviced = fadeState.Generation == rootRow.Generation && (fadeState.Verdict & kFadeVerdictServiced) != 0;
		if (serviced) {
			fadeHidden = (fadeState.Verdict & kFadeVerdictDrawn) == 0;
			if (a_count) {
				count.InterlockedAdd(kCountFadeTested, 1, scratch);
				if (fadeHidden)
					count.InterlockedAdd(kCountFadeHidden, 1, scratch);
			}
		}
	}
	// OnVisible snaps the fade of a root not in view last frame to 0 past its fade-out distance, so the draw is dropped.
	const float4 fade = float4(ObjectRow(a_objectIndex, kObjectFadeNodeRow).xyz, asfloat(a_inputs.Load(a_inputOffset + 60)));
	if (!serviced && (a_flags & kObjectFadeTest) != 0 && fade.w != 0.0 && FadeEye.w != 0.0) {
		const uint previous = frustumStamps.Load(a_objectIndex * 4);
		const bool wasInView = (previous & kFrustumStampMask) == ((VisibilityStamp - 1) & kFrustumStampMask);
		const bool wasHidden = wasInView && (previous & kFrustumFadeHidden) != 0;
		// FUN_14147b110 scales the distance by the camera's LOD factor for a positive fade distance, or by a constant for a negative one.
		const float distance = length(fade.xyz - FadeEye.xyz) * (fade.w > 0.0 ? FadeEye.w : 1.0);
		fadeHidden = distance > abs(fade.w) && (!wasInView || wasHidden);
		if (a_count) {
			count.InterlockedAdd(kCountFadeTested, 1, scratch);
			if (fadeHidden)
				count.InterlockedAdd(kCountFadeHidden, 1, scratch);
		}
	}
	frustumStamps.Store(a_objectIndex * 4, VisibilityStamp | (fadeHidden ? kFrustumFadeHidden : 0));
	return fadeHidden;
}

// The main camera's cull before the HZB. The frustum, then the portals and occluders its root is culled through.
bool MainFrustumRejected(uint objectIndex)
{
	if (MemberResultsIndex == 0)
		return true;
	StructuredBuffer<uint2> results = ResourceDescriptorHeap[MemberResultsIndex];
	const uint2 result = results[objectIndex];
	return result.x != VisibilityStamp || asint(result.y) != 1;
}

// Decal, forward and refraction inputs still need the main camera's cull for kAccumulated, so phase 1 stamps their frustum verdict only.
static const uint kStampOnlyViews = (1u << 8) | (1u << 9) | (1u << 10);

// Every slot is initialized before draw inputs claim their object. Shared skin-partition inputs perform just one bound test.
[numthreads(64, 1, 1)] void ClearPortalQueries(uint3 id : SV_DispatchThreadID) {
	LoadLatch();
	RWStructuredBuffer<PortalQuery> queries = ResourceDescriptorHeap[MemberQueriesIndex];
	RWStructuredBuffer<uint> claims = ResourceDescriptorHeap[MemberClaimsIndex];
	uint capacity, stride;
	queries.GetDimensions(capacity, stride);
	const uint objectIndex = id.x + id.y * (64u * 65535u);
	if (objectIndex >= capacity)
		return;
	PortalQuery query = (PortalQuery)0;
	query.Flags = 0x80000000u;
	query.Destination = objectIndex;
	query.Generation = VisibilityStamp;
	queries[objectIndex] = query;
	claims[objectIndex] = 0;
}

	[numthreads(64, 1, 1)] void PreparePortalQueries(uint3 id : SV_DispatchThreadID)
{
	LoadLatch();
	const uint draw = id.x;
	if (draw >= DrawCount)
		return;
	ByteAddressBuffer inputs = ResourceDescriptorHeap[InputsBuffer];
	const uint inputOffset = draw * kInputStride;
	if (ViewBits != 0 && (inputs.Load(inputOffset + 16) & (ViewBits | kStampOnlyViews)) == 0)
		return;
	const uint objectIndex = inputs.Load(inputOffset + 32);
	RWStructuredBuffer<PortalQuery> queries = ResourceDescriptorHeap[MemberQueriesIndex];
	RWStructuredBuffer<uint> claims = ResourceDescriptorHeap[MemberClaimsIndex];
	uint capacity, stride;
	queries.GetDimensions(capacity, stride);
	if (objectIndex >= capacity)
		return;  // ObjectFits prevents admission beyond the published batch.
	uint previous;
	InterlockedCompareExchange(claims[objectIndex], 0u, 1u, previous);
	if (previous != 0)
		return;
	PortalQuery query = (PortalQuery)0;
	query.Bound = ObjectRow(objectIndex, kObjectBoundRow);
	query.Flags = 0x80000000u;
	query.Destination = objectIndex;
	query.Generation = VisibilityStamp;
	query.Program = Culled(query.Bound.xyz, query.Bound.w) ? 0u : 1u;
	// The frame's published programs, 0 for frustum only.
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint programsIndex = latch.Load(LatchOffset + kLatchPortalPrograms);
	if (query.Program != 0 && programsIndex != 0 && RootProgramsIndex != 0) {
		const uint root = inputs.Load(inputOffset + 48);
		if (root != 0xFFFFFFFFu) {
			StructuredBuffer<uint> rootPrograms = ResourceDescriptorHeap[RootProgramsIndex];
			ByteAddressBuffer programs = ResourceDescriptorHeap[programsIndex];
			const uint program = rootPrograms[root];
			if (program < programs.Load(kPortalSetBytes)) {
				query.Program = program;
				query.Flags = 0;
				query.Bound.w = PortalRadiusWithMargin(query.Bound.w, CullMargin);
			}
		}
	}
	queries[objectIndex] = query;
}

// Puts phase 1's OccludedRetest inputs on phase 2's list, a wave at a time, growing phase 2's dispatch by whole groups of 64.
void AppendRetest(uint a_input, RWByteAddressBuffer a_count)
{
	const uint entries = WaveActiveCountBits(true);
	uint base = 0;
	if (WaveIsFirstLane()) {
		a_count.InterlockedAdd(kCountRetests, entries, base);
		const uint groups = (base + entries + 63) / 64 - (base + 63) / 64;
		if (groups != 0) {
			RWByteAddressBuffer dispatch = ResourceDescriptorHeap[RetestDispatchIndex];
			uint previous;
			dispatch.InterlockedAdd(0, groups, previous);
		}
	}
	base = WaveReadLaneFirst(base);
	RWByteAddressBuffer retests = ResourceDescriptorHeap[RetestsIndex];
	retests.Store((base + WavePrefixCountBits(true)) * 4, a_input);
}

// A DrawInput's 64 bytes loaded at once.
struct DrawInputWords
{
	uint4 words[4];
};

DrawInputWords LoadInputWords(ByteAddressBuffer a_inputs, uint a_draw)
{
	DrawInputWords loaded;
	[unroll] for (uint i = 0; i < 4; ++i)
		loaded.words[i] = a_inputs.Load4(a_draw * kInputStride + i * 16);
	return loaded;
}

// One input's culling and draws for the loaded view.
void BuildInput(uint draw, DrawInputWords a_words)
{
	ByteAddressBuffer inputs = ResourceDescriptorHeap[InputsBuffer];
	ByteAddressBuffer geometries = ResourceDescriptorHeap[GeometriesBuffer];
	RWByteAddressBuffer sequences = ResourceDescriptorHeap[SequencesIndex];
	RWByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];
	RWByteAddressBuffer visibility = ResourceDescriptorHeap[VisibilityIndex];

	const uint inputOffset = draw * kInputStride;
	const uint4 input = a_words.words[0];  // pipeline index, record index, geometry index, flags
	// A focus view casts only its target's members.
	if (FocusRoot != 0 && a_words.words[3].x != FocusRoot - 1)
		return;
	// A focus-only caster, a hidden actor part, is the focus views' alone.
	if (FocusRoot == 0 && (input.w & kObjectFocusOnly) == kObjectFocusOnly)
		return;
	const uint objectIndex = a_words.words[2].x;
	// A shadow view's key and rows words are its family's key and the shadow row.
	const uint keyWord = KeyWord();
	const uint drawKey = keyWord != 0 ? a_words.words[1][keyWord] : input.x;
	const uint rowsWord = keyWord != 0 ? a_words.words[3].y : input.y;
	if (drawKey == 0xFFFFFFFFu)
		return;  // no key in the view's family
	// The object word, with kObjectSunMiss when a sun-test pass meets no cascade. Counted for every such input to compare with the CPU.
	uint objectWord = objectIndex;
	if ((SunState & kSunTestOn) != 0 && (input.w & kObjectSunTest) != 0) {
		uint sunScratch;
		RWByteAddressBuffer sunCount = ResourceDescriptorHeap[CountIndex];
		sunCount.InterlockedAdd(kCountSunTested, 1, sunScratch);
		const float4 sunBound = ObjectRow(objectIndex, kObjectBoundRow);
		if (!InSunCascades(sunBound.xyz, sunBound.w + CullMargin)) {
			objectWord |= kObjectSunMiss;
			sunCount.InterlockedAdd(kCountSunMissed, 1, sunScratch);
		}
	}
	if (LocalShadowOffset != 0) {
		const float4 bound = ObjectRow(objectIndex, kObjectBoundRow);
		objectWord |= (LocalShadowMask(bound.xyz, bound.w + CullMargin, (input.w & kObjectLandscapeLights) != 0, objectIndex) & 0xFu) << kObjectLocalShadowShift;
	}
	// The drawable bit is the main family's. A shadow view draws every input its bits select.
	const bool drawable = keyWord != 0 || (input.w & kInputDrawable) != 0;
	uint scratch;
	const uint phase = CullPhase();

	// A shadow view draws one caster class, and the mode's inputs hold both.
	const bool volumetricCaster = (input.w & kObjectVolumetricOnly) != 0;
	if ((CastersOnly() && volumetricCaster) || (VolumetricOnly() && !volumetricCaster))
		return;
	if (phase == kPhaseSingle)
		count.InterlockedAdd(kCountCasterClass, 1, scratch);
	// The sun entry rule.
	if (SunEntry() && OutsideSunEntry(ObjectRow(objectIndex, kObjectSunEntryRow))) {
		count.InterlockedAdd(kCountSunEntryOut, 1, scratch);
		return;
	}
	if (MinRadius() && ObjectRow(objectIndex, kObjectBoundRow).w <= 32.0) {
		count.InterlockedAdd(kCountMinRadius, 1, scratch);
		return;
	}
	// A sun view's contribution test. The CPU folds the projection scale into MinCasterRadius.
	if (MinTexels() && ObjectRow(objectIndex, kObjectBoundRow).w < MinCasterRadius) {
		count.InterlockedAdd(kCountBelowTexels, 1, scratch);
		return;
	}
	// The engine casts no fading caster, so skip one under a fading or faded-out root.
	if (phase == kPhaseSingle && FadeRootsIndex != 0 &&
		(FadeOnVisible() ? FadedOutOfOcclusion(a_words.words[3].x) : RootFading(a_words.words[3].x))) {
		count.InterlockedAdd(kCountRootFading, 1, scratch);
		return;
	}

	// Tally for the CPU's terrain near check.
	if (phase == kPhaseOne && TerrainNear.w > 0.0 && TerrainGroup(input.w) == 1) {
		count.InterlockedAdd(kCountTerrainNearTested, 1, scratch);
		if (TerrainNearOf(ObjectRow(objectIndex, kObjectBoundRow)))
			count.InterlockedAdd(kCountTerrainNear, 1, scratch);
	}

	// Each decal writes its slot in every class sub-range of its group. Its own class gets the real sequence, others a zero-count filler.
	// So no stale slot is ever executed, and decals keep the engine's order within each class.
	const uint decalGroup = keyWord == 0 && (input.w & kObjectDecal) ? (input.w >> kObjectDecalGroupShift) & 3 : 0;
	if (decalGroup != 0) {
		if (phase != kPhaseColour && phase != kPhaseSingle)
			return;  // the depth segment never submits one
		const uint decalOrdinal = a_words.words[2].y;
		if (decalOrdinal >= DecalStride)
			return;  // never, the range holds every ordinal of the group
		bool culled = !drawable;
		if (drawable && CullMode() != 0) {
			count.InterlockedAdd(kCountDecalsTested, 1, scratch);
			const float4 bound = ObjectRow(objectIndex, kObjectBoundRow);
			culled = Culled(bound.xyz, bound.w) || (CullMode() >= 2 && Occluded(bound.xyz, bound.w));
			if (culled)
				count.InterlockedAdd(kCountDecalsCulled, 1, scratch);
		}
		// A drawable decal whose class has no sub-range in its group writes only fillers, and is counted.
		const uint classes = ClassCount();
		uint decalClass = 0, decalIndex = 0;
		bool decalPlanned = drawable && SetClassOf(input.x, decalClass, decalIndex);
		if (decalPlanned)
			decalPlanned = decalClass < classes && ClassRange(min(decalClass, classes - 1), decalGroup).y != 0;
		if (drawable && !decalPlanned)
			count.InterlockedAdd(kCountClassUnplanned, 1, scratch);
		const uint4 rows = RowsOf(input.y);
		const uint decalStreamIndex = a_words.words[2].w;
		// Blood decals copy their actor's dismember skin, so they take a slot per partition from their ordinal.
		const uint decalPartitions = a_words.words[2].z;
		const bool decalChain = (decalPartitions & kPartitionChain) != 0;
		const uint decalSpan = decalChain                                                 ? (decalPartitions & kPartitionChainCount) :
		                       (decalPartitions == 0 || decalPartitions == kNoPartitions) ? 1u :
		                                                                                    firstbithigh(decalPartitions & (kNoPartitions - 1)) + 1;
		// Plain decal buckets. A drawn partition is appended to its bucket's staging half keyed by its slot's ordinal, with no fillers.
		const bool decalBuckets = PlainBuckets(kDecalSection) != 0;
		uint decalBucket = 0, decalCountWord = 0;
		uint2 decalBucketRange = uint2(0, 0);
		if (decalBuckets && drawable) {
			if (!PlainBucketOf(kDecalSection, input.x, decalBucket, decalBucketRange, decalCountWord)) {
				if (decalPlanned)
					count.InterlockedAdd(kCountClassUnplanned, 1, scratch);
				decalBucketRange = uint2(0, 0);
			}
		}
		uint decalGeometry = input.z;
		[loop] for (uint k = 0; k < decalSpan; ++k)
		{
			if (decalOrdinal + k >= DecalStride)
				break;  // never, the range holds every slot of the group
			const bool partitionDrawn = decalChain || decalPartitions == 0 || ((decalPartitions >> k) & 1) != 0;
			const uint geometryOffset = (decalGeometry != kNoPartition ? decalGeometry : input.z) * kGeometryStride;
			const uint4 vertexBuffer = geometries.Load4(geometryOffset);
			const uint4 indexBuffer = geometries.Load4(geometryOffset + 16);
			const uint firstIndex = geometries.Load(geometryOffset + 32);
			const uint4 decalStream = decalStreamIndex != kNoStream ? geometries.Load4(decalStreamIndex * kGeometryStride) : vertexBuffer;
			const uint indexCount = (culled || !partitionDrawn || decalGeometry == kNoPartition) ? 0 : indexBuffer.w;
			if (decalBuckets) {
				if (indexCount != 0 && decalBucketRange.y != 0) {
					RWByteAddressBuffer classCounts = ResourceDescriptorHeap[ClassCountsIndex];
					uint taken;
					classCounts.InterlockedAdd(decalCountWord * 4, 1, taken);
					if (taken >= decalBucketRange.y) {
						count.InterlockedAdd(kCountClassOverflow, 1, scratch);
					} else {
						const uint staged = decalBucketRange.x + taken;
						ByteAddressBuffer decalFirsts = ResourceDescriptorHeap[PoolFirstsIndex];
						StoreShadowSequence(sequences, staged, 0, rows, objectWord, vertexBuffer, decalStream, indexBuffer, indexCount, firstIndex,
							decalFirsts.Load((decalGeometry != kNoPartition ? decalGeometry : input.z) * 4));
						// Sort key "depth" minus the slot's ordinal, so lower ordinals draw first.
						sequences.Store(staged * kSequenceStride + kSequenceInstanceCount, 0);
						sequences.Store(staged * kSequenceStride + kSequenceVertexOffset, 0);
						sequences.Store(staged * kSequenceStride + kSequenceFirstInstance, asuint(-float(decalOrdinal + k)));
					}
				}
				if (decalGeometry != kNoPartition)
					decalGeometry = geometries.Load(geometryOffset + 36);  // GeometryDraw::nextPartition
				continue;
			}
			[loop] for (uint c = 0; c < classes; ++c)
			{
				const uint2 range = ClassRange(c, decalGroup);
				if (decalOrdinal + k >= range.y)
					continue;  // no sub-range of the group
				const bool own = decalPlanned && c == decalClass;
				StoreSequence(sequences, range.x + decalOrdinal + k, own ? decalIndex : 0u, rows, objectWord, vertexBuffer, decalStream, indexBuffer,
					own ? indexCount : 0u, firstIndex);
			}
			if (decalGeometry != kNoPartition)
				decalGeometry = geometries.Load(geometryOffset + 36);  // GeometryDraw::nextPartition
		}
		// A decal is never drawn in depth, and drawn in colour unless culled.
		if (phase == kPhaseColour)
			visibility.Store(objectIndex * 4, (VisibilityStamp << kVisibilityStampShift) | (culled ? kVisibilityRejectedFinal : (kVisibilityVisible | kVisibilityColourDrawn)));
		return;
	}

	// A forward input is culled once, in the colour segment.
	const bool forwardInput = keyWord == 0 && (input.w & kObjectForward) != 0;
	const bool refractionInput = forwardInput && (a_words.words[1].x & kViewRefraction) != 0;
	// The first-person forward input is culled below with its other inputs.
	const bool firstPersonForward = forwardInput && !refractionInput && phase == kPhaseSingle && (ViewBits & kViewFirstPerson) != 0;
	if (forwardInput && !firstPersonForward) {
		const uint forwardCapacity = phase != kPhaseColour ? 0u : (refractionInput ? min(RefractionCapacity, PoolFirstsIndex) : ForwardCapacity);
		if (forwardCapacity == 0u)
			return;  // the depth segment never submits one
		bool forwardCulled = !drawable;
		if (drawable && CullMode() != 0) {
			count.InterlockedAdd(refractionInput ? kCountRefractionTested : kCountForwardTested, 1, scratch);
			const float4 bound = ObjectRow(objectIndex, kObjectBoundRow);
			forwardCulled = Culled(bound.xyz, bound.w);
			if (!forwardCulled) {
				count.InterlockedAdd(refractionInput ? kCountRefractionInView : kCountForwardInView, 1, scratch);
				if (CullMode() >= 2 && Occluded(bound.xyz, bound.w)) {
					forwardCulled = true;
					count.InterlockedAdd(refractionInput ? kCountRefractionOccluded : kCountForwardOccluded, 1, scratch);
				}
			}
		}
		// Refraction normals take no root fade, but nothing is drawn under a root whose OnVisible stopped, as the engine registers nothing there.
		if (refractionInput && !forwardCulled && FadeRootsIndex != 0 && FadeStatesIndex != 0) {
			const uint refractionRoot = a_words.words[3].x;
			if (refractionRoot != 0xFFFFFFFFu) {
				StructuredBuffer<FadeRootStatic> fadeRoots = ResourceDescriptorHeap[FadeRootsIndex];
				StructuredBuffer<FadeNodeState> fadeStates = ResourceDescriptorHeap[FadeStatesIndex];
				const FadeRootStatic rootRow = fadeRoots[refractionRoot];
				const FadeNodeState rootState = fadeStates[refractionRoot];
				if (rootState.Generation == rootRow.Generation && (rootState.Verdict & kFadeVerdictServiced) != 0 && (rootState.Verdict & kFadeVerdictDrawn) == 0) {
					forwardCulled = true;
					count.InterlockedAdd(kCountRefractionOccluded, 1, scratch);
				}
			}
		}
		visibility.Store(objectIndex * 4, (VisibilityStamp << kVisibilityStampShift) | (forwardCulled ? kVisibilityRejectedFinal : (kVisibilityVisible | kVisibilityColourDrawn)));
		if (forwardCulled)
			return;
	}

	// Reading the published decision keeps depth and colour drawing the same set. Under EQUAL, colour without depth is invisible and
	// depth without colour is a hole.
	const uint published = visibility.Load(objectIndex * 4);
	const bool publishedThisFrame = (published >> kVisibilityStampShift) == VisibilityStamp;
	const uint verdict = publishedThisFrame ? (published & kVisibilityVerdictMask) : kVisibilityVisible;
	if (phase == kPhaseTwo && !(publishedThisFrame && verdict == kVisibilityOccludedRetest))
		return;
	if (phase == kPhaseColour && !forwardInput && verdict != kVisibilityVisible)
		return;

	bool cullRejected = false, frustumRejected = false, occlusionRejected = false;
	if (phase != kPhaseColour && CullMode() != 0) {
		// Tells "the culling rejected nothing" from "the culling did not run".
		count.InterlockedAdd(kCountTested, 1, scratch);
		const float4 bound = ObjectRow(objectIndex, kObjectBoundRow);  // world centre, radius
		frustumRejected = phase == kPhaseOne ? MainFrustumRejected(objectIndex) :
			(phase != kPhaseTwo && (Parabolic() ? ParabolicRejected(bound.xyz, bound.w) : Culled(bound.xyz, bound.w)));
		// A portal-strict point light's view, gated by the caster's rooms and room frusta.
		if (!frustumRejected && phase == kPhaseSingle && StrictContext != 0)
			frustumRejected = StrictGateRejects(StrictGatesIndex, LightContextsIndex, ObjectGatesIndex, StrictContext - 1, objectIndex, bound.xyz, bound.w + CullMargin);
		if (frustumRejected) {
			count.InterlockedAdd(kCountCulled, 1, scratch);
		} else if (CullMode() >= 2 && Occluded(bound.xyz, bound.w)) {
			occlusionRejected = true;
			count.InterlockedAdd(kCountOccluded, 1, scratch);
		}
		cullRejected = frustumRejected || occlusionRejected;
	}
	// The feedback is frustum only, since occlusion does not stop the engine's OnVisible.
	bool fadeHidden = false;
	if (FrustumIndex != 0 && phase == kPhaseOne && !frustumRejected)
		fadeHidden = StampFrustum(inputs, inputOffset, input.w, objectIndex, true);
	// BSTreeNode::OnVisible draws nothing of a tree whose root is above the height limit.
	if (phase == kPhaseOne && !frustumRejected && (input.w & kObjectHeightTest) != 0) {
		const float3 root = ObjectRow(objectIndex, kObjectFadeNodeRow).xyz;
		fadeHidden = fadeHidden || root.z - TreeHeight.x > TreeHeight.y;
	}
	// Final, like the frustum's. Phase 2 never revisits it.
	cullRejected = cullRejected || fadeHidden;

	const bool draws = !cullRejected && drawable;

	// Phase 1 writes every candidate, so nothing stale reaches the colour segment.
	if (phase == kPhaseOne || phase == kPhaseTwo) {
		const uint decided = (frustumRejected || fadeHidden) ? kVisibilityRejectedFinal :
			(occlusionRejected ? (phase == kPhaseOne ? kVisibilityOccludedRetest : kVisibilityRejectedFinal) : kVisibilityVisible);
		visibility.Store(objectIndex * 4, (VisibilityStamp << kVisibilityStampShift) | decided | (draws ? kVisibilityDepthDrawn : 0));
		if (phase == kPhaseTwo && !occlusionRejected)
			count.InterlockedAdd(kCountRescuedByPhaseTwo, 1, scratch);
		if (phase == kPhaseOne && decided == kVisibilityOccludedRetest && RetestsIndex != 0)
			AppendRetest(draw, count);
	}
	if (phase == kPhaseColour && draws && !forwardInput) {
		if (publishedThisFrame)
			visibility.InterlockedOr(objectIndex * 4, kVisibilityColourDrawn, scratch);
		else
			visibility.Store(objectIndex * 4, (VisibilityStamp << kVisibilityStampShift) | kVisibilityColourDrawn | kVisibilityUnpublished);
	}

	if (cullRejected)
		return;
	// Cull-only inputs have nothing to draw.
	if (!drawable)
		return;
	const uint pipeline = DrawPipeline(drawKey);  // for a shadow view, the draw's bucket
	// A shadow view's bucket is its pipeline map entry. With a slot map, the pipeline slot's entry. kNoPipeline means none.
	uint bucketKey = pipeline;
	if (BucketMapOffset != 0) {
		ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
		bucketKey = latch.Load(BucketMapOffset + (rowsWord >> 16) * 4);
	}
	if (min(pipeline ^ kNoPipeline, bucketKey ^ kNoPipeline) == 0u)
		return;
	// kBucketForward entries take forward inputs only, and forward inputs only those.
	const bool forwardBucket = BucketMapOffset != 0 && (bucketKey & kBucketForward) != 0;
	bucketKey &= ~kBucketForward;
	if (forwardBucket != firstPersonForward)
		return;
	// The colour ranges are split by state class, one device-generated call per class sub-range. Class and index both come from the
	// input's set index, so they cannot disagree. A draw without a class sub-range is dropped and counted.
	const bool classed = (phase == kPhaseColour && keyWord == 0 && !refractionInput) || forwardBucket;
	uint setClass = 0, classIndex = 0;
	if (classed) {
		bool planned = SetClassOf(drawKey, setClass, classIndex);
		if (planned && !forwardBucket)
			planned = setClass < ClassCount();
		if (!planned) {
			count.InterlockedAdd(kCountClassUnplanned, 1, scratch);
			return;
		}
	}
	// A view's forward range has an entry per class.
	if (forwardBucket)
		bucketKey += setClass;
	// Terrain group 1. Near blocks go to the twin, drawn into Terrain Blending's depth and the main depth, far blocks to the main depth only.
	if ((bucketKey & kBucketTerrainPair) != 0) {
		bucketKey &= ~kBucketTerrainPair;
		if (TerrainNear.w > 0.0 && TerrainNearOf(ObjectRow(objectIndex, kObjectBoundRow)))
			bucketKey += 1;
	}
	// Colour terrain groups append to their own ranges, unsorted.
	const uint terrainGroup = phase == kPhaseColour && keyWord == 0 ? TerrainGroup(input.w) : 0;
	// Phase 2's few sequences draw from their own range, unsorted.
	const bool secondRange = phase == kPhaseTwo;
	const bool sorted = SortCountsIndex != 0 && !secondRange;
	uint2 bucket = uint2(0, 0);
	if (BucketTableOffset != 0) {
		ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
		bucket = latch.Load2(BucketTableOffset + bucketKey * 8);
	}
	// A class's draws sort within its own keys.
	const uint sortKey = classed ? ClassKeyBase(setClass) + classIndex : pipeline;
	if (sorted && sortKey >= kSortKeys)
		return;  // outside the sets, before any slot is taken, so the sorted range has no hole

	uint4 rows = RowsOf(rowsWord);
	// A shadow sequence carries its key slot's vertex layout and caster technique, so one bucket draws casters of every layout.
	uint slotTechnique = 0;
	if (SlotLayoutsOffset != 0) {
		ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
		const uint4 entry = latch.Load4(SlotLayoutsOffset + drawKey * 16);
		rows.xy = entry.xy;
		slotTechnique = entry.z;
	}

	// One draw per partition the mask names. All share one visibility word.
	uint copyPartitions;
	const uint basePartitions = LodPartitions(a_words.words[2].z, a_words.words[3].z, a_words.words[3].x, keyWord == 0, copyPartitions);
	if (basePartitions == kNoPartitions)
		return;
	const uint partitions = basePartitions | copyPartitions;
	// A face shape's positions are the whole shape's, so the second stream is the same for every partition.
	const uint streamIndex = a_words.words[2].w;
	const uint4 stream = streamIndex != kNoStream ? geometries.Load4(streamIndex * kGeometryStride) : uint4(0, 0, 0, 0);
	uint geometryIndex = input.z;
	const bool chain = (partitions & kPartitionChain) != 0;
	const uint links = chain ? (partitions & kPartitionChainCount) : kMaxPartitions;
	// This input's rows in its shadow family. Dropped before any bucket slot is taken, so a bucket has no hole.
	uint casterFirst = 0, casterMask = 0, casterSpan = 0;
	if (CasterRowsIndex != 0) {
		// Its draws are bucketed.
		if (BucketTableOffset == 0)
			return;
		ByteAddressBuffer casterFirsts = ResourceDescriptorHeap[CasterFirstsIndex];
		casterFirst = casterFirsts.Load(objectIndex * 4);
		if (casterFirst == 0xFFFFFFFFu) {
			count.InterlockedAdd(kCountCasterRowsLost, 1, scratch);
			return;
		}
		casterMask = CasterPartitionMask(a_words.words[2].z, a_words.words[3].z);
		casterSpan = CasterSpan(casterMask);
		const uint4 keys = a_words.words[1];
		casterFirst += casterSpan * ((keyWord >= 2 && keys.y != 0xFFFFFFFFu ? 1u : 0u) + (keyWord == 3 && keys.z != 0xFFFFFFFFu ? 1u : 0u));
	}
	[loop] for (uint partition = 0; partition < links && geometryIndex != kNoPartition; ++partition)
	{
		const uint geometryOffset = geometryIndex * kGeometryStride;
		if (chain || partitions == 0 || ((partitions >> partition) & 1) != 0) {
			const uint drawWord = ((copyPartitions >> partition) & 1) != 0 ? (objectWord | kObjectCrossfadeCopy) : objectWord;
			const uint4 vertexBuffer = geometries.Load4(geometryOffset);      // address lo, hi, size, stride
			const uint4 indexBuffer = geometries.Load4(geometryOffset + 16);  // address lo, hi, size, index count
			const uint firstIndex = geometries.Load(geometryOffset + 32);

			// Each range holds every draw it can receive. The guards keep each inside its range regardless.
			const uint4 secondStream = streamIndex != kNoStream ? stream : vertexBuffer;
			uint slot;
			if (BucketTableOffset != 0) {
				// Bucketed draws take the bucket's next slot. The slot is the instance the pulled vertex stage finds the sequence by.
				const uint casterOrdinal = CasterRowsIndex != 0 ? CasterOrdinal(casterMask, partition) : 0u;
				if (CasterRowsIndex != 0 && casterOrdinal >= casterSpan) {
					count.InterlockedAdd(kCountCasterRowsLost, 1, scratch);
					geometryIndex = geometries.Load(geometryOffset + 36);
					continue;
				}
				RWByteAddressBuffer bucketCounts = ResourceDescriptorHeap[BucketCountsIndex];
				bucketCounts.InterlockedAdd(bucketKey * 4, 1, slot);
				if (slot < bucket.y) {
					uint drawn;
					count.InterlockedAdd(phase == kPhaseTwo ? kCountDrawnPhaseTwo : kCountDrawn, 1, drawn);
					ByteAddressBuffer poolFirsts = ResourceDescriptorHeap[PoolFirstsIndex];
					if (forwardBucket) {
						// First-person forward staging, a main-format sequence with its sort key.
						StoreSequence(sequences, bucket.x + slot, classIndex, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex);
						StageForwardKey(sequences, bucket.x + slot, objectIndex);
					} else if (CasterRowsIndex != 0) {
						const uint casterRow = casterFirst + casterOrdinal;
						RWByteAddressBuffer casterRows = ResourceDescriptorHeap[CasterRowsIndex];
						StoreCasterRow(casterRows, casterRow, slotTechnique, rows, drawWord, vertexBuffer, secondStream);
						StoreShadowDraw(sequences, bucket.x + slot, casterRow, indexBuffer.w, firstIndex, poolFirsts.Load(geometryIndex * 4));
					} else {
						StoreShadowSequence(sequences, bucket.x + slot, slot, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w,
							firstIndex, poolFirsts.Load(geometryIndex * 4));
						sequences.Store((bucket.x + slot) * kSequenceStride, slotTechnique);
					}
				}
			} else if (refractionInput) {
				// A pooled refraction sequence, found by its slot as instance.
				count.InterlockedAdd(kCountRefraction, 1, slot);
				if (slot >= RefractionCapacity)
					return;
				ByteAddressBuffer refractionFirsts = ResourceDescriptorHeap[PoolFirstsIndex];
				StoreShadowSequence(sequences, ForwardBase + 2 * ForwardCapacity + slot, slot, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w,
					firstIndex, refractionFirsts.Load(geometryIndex * 4));
			} else if (forwardInput) {
				// Forward draws go to their class's staging half or their forward bucket, with the sort key.
				count.InterlockedAdd(kCountForward, 1, scratch);
				if (PlainBuckets(kForwardSection) != 0) {
					uint bucket, countWord;
					uint2 range;
					if (!PlainBucketOf(kForwardSection, drawKey, bucket, range, countWord)) {
						count.InterlockedAdd(kCountClassUnplanned, 1, scratch);
						return;
					}
					RWByteAddressBuffer classCounts = ResourceDescriptorHeap[ClassCountsIndex];
					uint taken;
					classCounts.InterlockedAdd(countWord * 4, 1, taken);
					if (taken >= range.y) {
						count.InterlockedAdd(kCountClassOverflow, 1, scratch);
						return;
					}
					slot = range.x + taken;
					ByteAddressBuffer forwardFirsts = ResourceDescriptorHeap[PoolFirstsIndex];
					StoreShadowSequence(sequences, slot, 0, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex,
						forwardFirsts.Load(geometryIndex * 4));
				} else {
					if (!TakeClassSlot(setClass, kClassRangeForward, slot))
						return;
					StoreSequence(sequences, slot, classIndex, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex);
				}
				StageForwardKey(sequences, slot, objectIndex);
			} else if (terrainGroup != 0) {
				// Its class's sub-range of its terrain group's range, unsorted.
				count.InterlockedAdd(kCountTerrainGroup1 + (terrainGroup - 1) * 4, 1, scratch);
				if (PlainBuckets(kTerrainSection) != 0) {
					// Or its plain terrain bucket, as a pooled sequence.
					uint bucket, countWord;
					uint2 range;
					if (!PlainBucketOf(kTerrainSection, drawKey, bucket, range, countWord)) {
						count.InterlockedAdd(kCountClassUnplanned, 1, scratch);
						return;
					}
					RWByteAddressBuffer classCounts = ResourceDescriptorHeap[ClassCountsIndex];
					uint taken;
					classCounts.InterlockedAdd(countWord * 4, 1, taken);
					if (taken >= range.y) {
						count.InterlockedAdd(kCountClassOverflow, 1, scratch);
						return;
					}
					ByteAddressBuffer terrainFirsts = ResourceDescriptorHeap[PoolFirstsIndex];
					StoreShadowSequence(sequences, range.x + taken, taken, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex,
						terrainFirsts.Load(geometryIndex * 4));
				} else {
					if (!TakeClassSlot(setClass, 3 + terrainGroup, slot))
						return;
					StoreSequence(sequences, slot, classIndex, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex);
				}
			} else if (classed && PlainBuckets(kMainSection) != 0) {
				// Plain main buckets, pooled and unsorted.
				uint bucket, countWord;
				uint2 range;
				if (!PlainBucketOf(kMainSection, drawKey, bucket, range, countWord)) {
					count.InterlockedAdd(kCountClassUnplanned, 1, scratch);
					return;
				}
				RWByteAddressBuffer classCounts = ResourceDescriptorHeap[ClassCountsIndex];
				uint taken;
				classCounts.InterlockedAdd(countWord * 4, 1, taken);
				if (taken >= range.y) {
					count.InterlockedAdd(kCountClassOverflow, 1, scratch);
					return;
				}
				count.InterlockedAdd(kCountDrawn, 1, scratch);
				ByteAddressBuffer mainFirsts = ResourceDescriptorHeap[PoolFirstsIndex];
				StoreShadowSequence(sequences, range.x + taken, taken, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex,
					mainFirsts.Load(geometryIndex * 4));
			} else if (classed) {
				// Colour draws go to their class's main sub-range, sorted by pipeline within the class. Capacity is checked first so staging
				// takes only what the scatter places.
				uint classSlot;
				if (!TakeClassSlot(setClass, kClassRangeMain, classSlot))
					return;
				if (sorted) {
					RWByteAddressBuffer sortCounts = ResourceDescriptorHeap[SortCountsIndex];
					RWByteAddressBuffer staging = ResourceDescriptorHeap[SortStagingIndex];
					RWByteAddressBuffer ranks = ResourceDescriptorHeap[SortRanksIndex];
					count.InterlockedAdd(kCountDrawn, 1, slot);
					if (slot >= PhaseTwoBase)
						return;  // never, the classes' main sub-ranges hold at most PhaseTwoBase draws
					// The scatter reads key and rank from the rank word, and the class's first slot and key from the staged words, which it restores.
					uint rank;
					sortCounts.InterlockedAdd(sortKey * 4, 1, rank);
					ranks.Store(slot * 4, (sortKey << kSortRankBits) | rank);
					StoreSequence(staging, slot, classIndex, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex);
					staging.Store(slot * kSequenceStride + kSequenceInstanceCount, ClassRange(setClass, kClassRangeMain).x);
					staging.Store(slot * kSequenceStride + kSequenceFirstInstance, ClassKeyBase(setClass));
				} else {
					count.InterlockedAdd(kCountDrawn, 1, scratch);
					StoreSequence(sequences, classSlot, classIndex, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex);
				}
			} else {
				// Any other unbucketed draw, appended unsorted. There are none today.
				count.InterlockedAdd(secondRange ? kCountDrawnPhaseTwo : kCountDrawn, 1, slot);
				if (slot >= PhaseTwoBase)
					return;
				StoreSequence(sequences, slot + (secondRange ? PhaseTwoBase : 0), pipeline, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w,
					firstIndex);
			}
		}
		if (!chain && (partitions >> (partition + 1)) == 0)
			break;
		geometryIndex = geometries.Load(geometryOffset + 36);  // GeometryDraw::nextPartition
	}
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	LoadLatch();
	uint draw = dispatchID.x;
	// Phase 2 runs over phase 1's retest list, one entry per input left OccludedRetest.
	if (CullPhase() == kPhaseTwo && RetestsIndex != 0) {
		RWByteAddressBuffer retestCount = ResourceDescriptorHeap[CountIndex];
		if (draw >= retestCount.Load(kCountRetests))
			return;
		ByteAddressBuffer retests = ResourceDescriptorHeap[RetestsIndex];
		draw = retests.Load(draw * 4);
	}
	if (draw >= DrawCount)
		return;

	ByteAddressBuffer inputs = ResourceDescriptorHeap[InputsBuffer];
	const uint inputOffset = draw * kInputStride;
	// Test view types against the input's view mask before reading anything else.
	const uint viewMask = inputs.Load(inputOffset + 16);
	if (ViewBits != 0 && (viewMask & ViewBits) == 0) {
		if (FrustumIndex != 0 && CullPhase() == kPhaseOne && (viewMask & kStampOnlyViews) != 0) {
			const uint stampObject = inputs.Load(inputOffset + 32);
			if (CullMode() == 0 || !MainFrustumRejected(stampObject))
				StampFrustum(inputs, inputOffset, inputs.Load(inputOffset + 12), stampObject, false);
		}
		return;
	}
	BuildInput(draw, LoadInputWords(inputs, draw));
}

// A row of the views' table, a view's latch offset and outputs for one dispatch per view.
struct ShadowBuildView
{
	uint LatchAt;        // bytes into the frame slot's region
	uint DrawArgs;       // RWByteAddressBuffer: the slot's draws' arguments (StoreShadowDraw)
	uint Counts;         // RWByteAddressBuffer: the slot's counter words
	uint BucketCounts;   // RWByteAddressBuffer: the slot's bucket counts
	uint SequenceDraws;  // the slot's draws (PhaseTwoBase)
	uint CasterRows;     // RWByteAddressBuffer: the rows every view shares (StoreCasterRow)
	uint CasterFirsts;   // ByteAddressBuffer: each object's first row (ShadowCasterRows)
};

void LoadShadowView(ShadowBuildView a_view)
{
	SequencesIndex = a_view.DrawArgs;
	CountIndex = a_view.Counts;
	LatchOffset = PushLatchOffset + a_view.LatchAt;
	PhaseTwoBase = a_view.SequenceDraws;
	BucketCountsIndex = a_view.BucketCounts;
	CasterRowsIndex = a_view.CasterRows;
	CasterFirstsIndex = a_view.CasterFirsts;
	LoadViewLatch();
}

// All shadow epoch views in one dispatch, a thread per input and view. y is the views' table row, so a group shares its view.
// A view's sequences take slots in its atomics' order, as in main. A y past the rows does nothing.
[numthreads(64, 1, 1)] void ShadowViews(uint3 dispatchID : SV_DispatchThreadID) {
	if (dispatchID.y >= PushCountIndex)
		return;
	StructuredBuffer<ShadowBuildView> views = ResourceDescriptorHeap[PushSequencesIndex];
	LoadShadowView(views[dispatchID.y]);
	const uint draw = dispatchID.x;
	// A zero latch has no inputs.
	if (draw >= DrawCount)
		return;
	ByteAddressBuffer inputs = ResourceDescriptorHeap[InputsBuffer];
	// A shadow view stamps nothing, since it is single phase.
	if (ViewBits != 0 && (inputs.Load(draw * kInputStride + 16) & ViewBits) == 0)
		return;
	BuildInput(draw, LoadInputWords(inputs, draw));
}

// Hands out the frame's caster rows before the shadow epoch's culling. Each caster gets consecutive rows, one per partition per
// view family it has a key in. An object past the rows gets none and its views count it.
[numthreads(64, 1, 1)] void ShadowCasterRows(uint3 dispatchID : SV_DispatchThreadID) {
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint2 list = latch.Load2(PushLatchOffset);
	if (dispatchID.x >= list.y)
		return;
	ByteAddressBuffer inputs = ResourceDescriptorHeap[list.x];
	const uint inputOffset = dispatchID.x * kInputStride;
	const uint3 keys = inputs.Load3(inputOffset + 20);
	const uint families = (keys.x != 0xFFFFFFFFu ? 1u : 0u) + (keys.y != 0xFFFFFFFFu ? 1u : 0u) + (keys.z != 0xFFFFFFFFu ? 1u : 0u);
	if (families == 0)
		return;
	const uint rows = families * CasterSpan(CasterPartitionMask(inputs.Load(inputOffset + 40), inputs.Load(inputOffset + 56)));
	RWByteAddressBuffer used = ResourceDescriptorHeap[PushCountIndex];
	uint first;
	used.InterlockedAdd(0, rows, first);
	// On D3D12 a raw GetDimensions of a structured view gives elements, not bytes.
	RWByteAddressBuffer firsts = ResourceDescriptorHeap[PushSequencesIndex];
	firsts.Store(inputs.Load(inputOffset + 32) * 4, first + rows <= PushPhaseTwoBase ? first : 0xFFFFFFFFu);
}

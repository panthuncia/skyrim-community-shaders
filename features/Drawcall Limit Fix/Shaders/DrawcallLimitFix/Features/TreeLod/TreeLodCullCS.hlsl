// Drawcall Limit Fix. Culls tree LOD instances for the main camera in the depth segment's two phases, like every other object.
//
// Phase 1 runs one thread per instance record, shape slot id / 75, record id % 75. Records past the slot's count, or hidden with
// scale 0, draw nothing. The rest are tested against the frustum and last frame's HZB. Visible ones are appended for phase 1's
// depth draw, occluded ones go on the retest list.
// Phase 2 retests each entry against the rebuilt HZB and appends survivors after phase 1's. The colour pass draws both phases'.
// The depth commit zeroes the counts.
#include "DrawcallLimitFix/CullEye.hlsli"
#include "DrawcallLimitFix/HzbTest.hlsli"

// Must match TreeLodCullConstants in TreeLodTables.h.
cbuffer TreeLodCullConstants : register(b0)
{
	uint ShapesIndex;     // ByteAddressBuffer: TreeLod::ShapeRow per shape slot
	uint InstancesIndex;  // ByteAddressBuffer: TreeLod::Instance, kMaxGroupInstances per shape slot
	uint VisibleIndex;    // RWByteAddressBuffer, the header, the visible indices, the retest indices
	uint LatchIndex;      // ByteAddressBuffer, the depth segment's BuildDrawsLatch
	uint LatchOffset;
	uint DrawIndex;       // ByteAddressBuffer, TreeLod::DrawRow, whose shapeSlots bounds the slots, 0 when not drawing
	uint Phase;           // 1 or 2
	uint RetestOffset;    // bytes to the retest list's first index
	uint HzbIndex;        // Texture2D<float>, 0 when there is no HZB
	uint HzbSizePacked;   // mip 0: width in the low 16 bits, height in the high 16
	uint HzbMips;
	uint Padding0;
}

static const uint kMaxGroupInstances = 75;
static const uint kShapeRowBytes = 32;
static const uint kInstanceBytes = 32;
// TreeLod::VisibleHeader. The three draws' DrawInstanced arguments, the retest count, then the visible indices.
static const uint kPhaseOneInstances = 4;
static const uint kPhaseTwoInstances = 20;
static const uint kColourInstances = 36;
static const uint kRetestCount = 48;
static const uint kVisibleHeaderBytes = 64;

// The depth segment's camera-relative view-projection. A world position projects less the latch's eye.
float4x4 LatchViewProj()
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	return float4x4(asfloat(latch.Load4(LatchOffset + 32)), asfloat(latch.Load4(LatchOffset + 48)),
		asfloat(latch.Load4(LatchOffset + 64)), asfloat(latch.Load4(LatchOffset + 80)));
}

// The sphere test against the main camera's frustum, as FadeStateCS's InView. NoNearPlane drops the near plane.
bool InView(float4x4 a_viewProj, float3 a_centre, float a_radius)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint cullFlags = latch.Load(LatchOffset + 16);
	const float4 r0 = a_viewProj[0], r1 = a_viewProj[1], r2 = a_viewProj[2], r3 = a_viewProj[3];
	const precise float3 centre = a_centre - LatchCullEye(latch, LatchOffset).xyz;
	float4 planes[6] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2 };
	[unroll] for (uint p = 0; p < 6; ++p) {
		if (p == 4 && (cullFlags & 0x200u) != 0)
			continue;
		const float length = sqrt(dot(planes[p].xyz, planes[p].xyz));
		if (length <= 0.0f)
			continue;
		if ((dot(planes[p].xyz, centre) + planes[p].w) / length < -a_radius)
			return false;
	}
	return true;
}

// Tests the HZB only in cull mode 2 with an HZB bound.
bool Occluded(float4x4 a_viewProj, float3 a_centre, float a_radius)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	if ((latch.Load(LatchOffset + 16) & 0xFu) < 2)
		return false;
	HzbView view;
	view.ViewProj = a_viewProj;
	view.Eye = LatchCullEye(latch, LatchOffset).xyz;
	view.Index = HzbIndex;
	view.BaseSize = HzbUnpackSize(HzbSizePacked);
	view.Mips = HzbMips;
	view.UvScale = HzbUnpackUvScale(latch.Load(LatchOffset + 24));
	return HzbTest(view, a_centre, a_radius).Occluded;
}

// The engine's group AABB pads each instance by its type's larger extent times scale, holding the billboard any way it turns.
// HzbTest projects the sphere's cube, which is that box. The frustum's sphere holds it at sqrt(3) times the radius.
bool Bound(uint a_index, out float3 a_centre, out float a_radius)
{
	const uint slot = a_index / kMaxGroupInstances;
	const uint record = a_index % kMaxGroupInstances;
	ByteAddressBuffer shapes = ResourceDescriptorHeap[ShapesIndex];
	const uint shapeBase = slot * kShapeRowBytes;
	a_centre = float3(0, 0, 0);
	a_radius = 0;
	if (record >= shapes.Load(shapeBase + 16))
		return false;
	const float3 translate = asfloat(shapes.Load3(shapeBase));
	const float extent = asfloat(shapes.Load(shapeBase + 20));
	ByteAddressBuffer instances = ResourceDescriptorHeap[InstancesIndex];
	const uint2 words = instances.Load2(a_index * kInstanceBytes);  // x, y | z, scale
	const float scale = f16tof32(words.y >> 16);
	if (scale <= 0.0f)
		return false;
	a_centre = translate + float3(f16tof32(words.x), f16tof32(words.x >> 16), f16tof32(words.y));
	a_radius = extent * scale;
	return true;
}

[numthreads(64, 1, 1)] void main(uint a_id : SV_DispatchThreadID)
{
	RWByteAddressBuffer visible = ResourceDescriptorHeap[VisibleIndex];
	uint index;
	if (Phase == 2) {
		if (a_id >= visible.Load(kRetestCount))
			return;
		index = visible.Load(RetestOffset + a_id * 4);
	} else {
		ByteAddressBuffer draw = ResourceDescriptorHeap[DrawIndex];
		if (a_id / kMaxGroupInstances >= draw.Load(44))
			return;
		index = a_id;
	}
	float3 centre;
	float radius;
	if (!Bound(index, centre, radius))
		return;
	const float4x4 viewProj = LatchViewProj();
	// Phase 2 already has its frustum answer from phase 1 and only revisits occlusion.
	if (Phase != 2 && !InView(viewProj, centre, radius * 1.7320508))
		return;
	uint at;
	if (Occluded(viewProj, centre, radius)) {
		if (Phase != 2) {
			visible.InterlockedAdd(kRetestCount, 1u, at);
			visible.Store(RetestOffset + at * 4, index);
		}
		return;
	}
	uint scratch;
	visible.InterlockedAdd(kColourInstances, 1u, scratch);
	if (Phase == 2) {
		// After phase 1's. DistantTree.hlsl reads phase 1's count, where phase 2's draw starts.
		visible.InterlockedAdd(kPhaseTwoInstances, 1u, at);
		at += visible.Load(kPhaseOneInstances);
	} else {
		visible.InterlockedAdd(kPhaseOneInstances, 1u, at);
	}
	visible.Store(kVisibleHeaderBytes + at * 4, index);
}

// Drawcall Limit Fix: writes the indirect draw sequences (DrawSequence, Records.h) of this frame's draws
// from the per-draw inputs and the geometry table, appending them with an atomic count. Compiled to SPIR-V
// for the render graph (BasicRHI's descriptor-heap ABI); buffers are fetched from the descriptor heap by index.

cbuffer BuildDrawsConstants : register(b0)
{
	uint DrawCount;
	uint InputsIndex;      // ByteAddressBuffer: DrawInput[DrawCount]
	uint GeometriesIndex;  // ByteAddressBuffer: GeometryDraw[]
	uint SequencesIndex;   // RWByteAddressBuffer: DrawSequence[]
	uint CountIndex;       // RWByteAddressBuffer: uint (zeroed before the dispatch)
	uint RecordsAddressLo;  // device address of DrawBindings[0]
	uint RecordsAddressHi;
	uint RecordStride;
	// Phase 4 culling. Bits 0-3 are the mode (0 off, 1 frustum, 2 frustum then the HZB), bits 4-7 the
	// phase, and bit 8 RequireNativeVisible. They share a word because these are push constants and the
	// block is at Vulkan's guaranteed 128 bytes.
	uint CullFlags;
	// RWByteAddressBuffer: one uint per object in the frame's tables, written by the depth segment's two
	// culling phases and read by the colour segment. This is how the colour pass draws exactly what the
	// depth passes drew: the decision is made once, in the depth segment, and published here, instead of
	// each segment testing for itself and quietly disagreeing.
	uint VisibilityIndex;
	// Stamps the verdicts this frame publishes. A word that does not carry the current stamp was not
	// written this frame, and the colour segment then treats the object as visible rather than trusting a
	// stale verdict. Visible is the safe default: drawing something the depth passes did not cover means
	// the colour draw's EQUAL test fails and the native pass shades it, while believing a stale "hidden"
	// would drop an object that has depth and no other draw left to shade it.
	uint VisibilityStamp;
	uint CullPadding;  // keeps ViewProj on a 16-byte boundary
	// The main pass's view-projection with the camera translation already folded into it, so a bound's
	// absolute world position projects directly. The camera (posAdjust) used to be a separate float3 here;
	// folding it in freed the words VisibilityIndex now uses.
	row_major float4x4 ViewProj;
	// Occlusion culling against the hierarchical depth buffer built at the end of the depth pass
	// (HzbCS.hlsl). HzbIndex is zero when there is no HZB, which includes the first frame.
	//
	// Sizes are packed into single words because these are push constants: Vulkan only guarantees 128
	// bytes of them, and the block is exactly that with this packing.
	uint HzbIndex;
	uint HzbSizePacked;  // mip 0: width in the low 16 bits, height in the high 16
	uint HzbMips;
	// The HZB covers a power-of-two area that is larger than the rendered image, so a texture coordinate in
	// the image is NOT one in the HZB. This is the ratio between them, per axis, as 16-bit fixed point.
	// Leaving it out makes the culling sample the padding, which is all far plane, and the HZB then looks
	// uniformly empty however correctly it was built.
	uint HzbUvScalePacked;
}

uint CullMode() { return CullFlags & 0xF; }
// 1: the depth segment's first phase, against the HZB left by the previous frame. 2: its second phase,
// against the HZB just rebuilt from this frame's depth, over what phase 1 rejected. 3: the colour segment,
// which does not test anything and just reads what the two phases decided.
uint CullPhase() { return (CullFlags >> 4) & 0xF; }
static const uint kPhaseSingle = 0;
static const uint kPhaseOne = 1;
static const uint kPhaseTwo = 2;
static const uint kPhaseColour = 3;

// Visibility values. Frustum rejection is final - nothing about a later depth buffer can bring an object
// back inside the frustum - while an occlusion rejection in phase 1 is provisional, because phase 1 tested
// against a depth buffer from the previous frame.
static const uint kVisibilityOccludedRetest = 0;
static const uint kVisibilityVisible = 1;
static const uint kVisibilityRejectedFinal = 2;
// When set, only objects the engine's own culling kept (kObjectNativeVisible) may be drawn, which is what
// the native loop skips and therefore what the frame has to contain. When clear, the GPU culling alone
// decides and DCLF draws objects the engine culled. The counters are written either way, so the
// cross-tabulation below measures the culling against the engine even while the gate is on.
bool RequireNativeVisible() { return (CullFlags & 0x100) != 0; }

// Not a static global: its value comes from a constant buffer, so it has to be read where it is used.
uint2 HzbBaseSize() { return uint2(HzbSizePacked & 0xFFFF, HzbSizePacked >> 16); }
float2 HzbUvScale() { return float2(HzbUvScalePacked & 0xFFFF, HzbUvScalePacked >> 16) / 65535.0; }

// Object flags (Records.h), as the draw input carries them.
static const uint kObjectNativeVisible = 1u << 3;
// Set by the epoch rather than by the object: this input carries a usable bindings record, so a sequence
// may be written for it. The depth segment submits an input for EVERY candidate so that the culling covers
// them all and the published visibility is complete, but it only builds records for the ones it is allowed
// to draw - which is what the native loop is skipping. The rest are cull-only.
static const uint kInputDrawable = 1u << 16;

// Counter words in the count buffer (zeroed before the dispatch).
static const uint kCountDrawn = 0;          // sequences written
static const uint kCountCulled = 4;         // rejected by this frame's culling
static const uint kCountTested = 8;         // looked at by the culling at all
static const uint kCountEngineCulled = 12;  // candidates the engine culled and the gate dropped
static const uint kCountFalseNegative = 16; // the engine kept it and the culling rejected it: a defect
static const uint kCountRescued = 20;       // the engine culled it and the culling kept it
static const uint kCountOccluded = 24;      // rejected by the HZB rather than by the frustum
// What the HZB actually held under the objects that were tested, so that a suspicious rejection count can
// be told apart from an HZB that is uniformly empty or uniformly full without a texture readback.
static const uint kCountHzbNear = 28;       // the footprint's farthest depth was ~0: the build wrote nothing
static const uint kCountHzbFar = 32;        // it was ~1: all far plane, so nothing can ever be occluded
static const uint kCountHzbSampled = 36;    // footprints sampled, to give the two above a denominator
// One rejection, recorded in full (see Occluded).
static const uint kSampleTaken = 40;
static const uint kSampleFarthest = 44;
static const uint kSampleNearestZ = 48;
static const uint kSampleUvMin = 52;
static const uint kSampleUvMax = 56;
static const uint kSampleMip = 60;
static const uint kCountOccludedVisible = 64;  // the HZB rejected it and the engine had kept it: the win
static const uint kCountDrawnPhaseTwo = 68;    // sequences phase 2 appended, and the count its draw reads
static const uint kCountRescuedByPhaseTwo = 72;  // objects phase 1 rejected and the rebuilt HZB brought back

// Phase 2 writes into the far half of the sequence buffer. Its draw is recorded on the CPU, which cannot
// know how many sequences phase 1 wrote, so the two need ranges that are fixed in advance rather than one
// range shared by an atomic counter.
static const uint kPhaseTwoSequenceBase = 16384;  // kMaxDraws

// DrawInput: 32 bytes (pipeline/record/geometry/flags, then the world-space bounding sphere).
static const uint kInputStride = 40;
// GeometryDraw: 40 bytes (vertex buffer view, index buffer view, index count, first index).
static const uint kGeometryStride = 40;
// DrawSequence: 64 bytes, 4-byte packed.
static const uint kSequenceStride = 64;
static const uint kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT

// The bounding sphere's world-space AABB, projected corner by corner. The box contains the sphere, so
// every test built on it errs towards keeping the object: an object is only rejected when all eight
// corners are outside the same clip plane, which no visible object can be.
//
// A corner behind the near plane makes the projection meaningless, so the object is kept.
bool Culled(float3 boundCentre, float boundRadius)
{
	const float3 centre = boundCentre;
	float4 planes = float4(1, 1, 1, 1);  // all-corners-outside accumulators: -x, +x, -y, +y
	float2 depthPlanes = float2(1, 1);   // near, far
	[unroll] for (uint corner = 0; corner < 8; ++corner) {
		const float3 offset = float3((corner & 1) ? boundRadius : -boundRadius,
			(corner & 2) ? boundRadius : -boundRadius,
			(corner & 4) ? boundRadius : -boundRadius);
		const float4 clip = mul(ViewProj, float4(centre + offset, 1.0));
		if (clip.w <= 1e-4)
			return false;  // crosses the near plane: treat as visible
		planes.x *= (clip.x < -clip.w) ? 1 : 0;
		planes.y *= (clip.x > clip.w) ? 1 : 0;
		planes.z *= (clip.y < -clip.w) ? 1 : 0;
		planes.w *= (clip.y > clip.w) ? 1 : 0;
		depthPlanes.x *= (clip.z < 0) ? 1 : 0;
		depthPlanes.y *= (clip.z > clip.w) ? 1 : 0;
	}
	return any(planes != 0) || any(depthPlanes != 0);
}

// The bounding sphere's screen-space extent and its nearest depth, in the same clip space the draws use.
// Returns false when the projection cannot be trusted - a corner behind the near plane - so that the
// object is kept.
bool ScreenExtent(float3 boundCentre, float boundRadius, out float2 uvMin, out float2 uvMax, out float nearestZ)
{
	uvMin = float2(1, 1);
	uvMax = float2(0, 0);
	nearestZ = 1;
	const float3 centre = boundCentre;
	float2 ndcMin = float2(1e30, 1e30);
	float2 ndcMax = float2(-1e30, -1e30);
	float minZ = 1e30;
	[unroll] for (uint corner = 0; corner < 8; ++corner) {
		const float3 offset = float3((corner & 1) ? boundRadius : -boundRadius,
			(corner & 2) ? boundRadius : -boundRadius,
			(corner & 4) ? boundRadius : -boundRadius);
		const float4 clip = mul(ViewProj, float4(centre + offset, 1.0));
		if (clip.w <= 1e-4)
			return false;
		const float3 ndc = clip.xyz / clip.w;
		ndcMin = min(ndcMin, ndc.xy);
		ndcMax = max(ndcMax, ndc.xy);
		minZ = min(minZ, ndc.z);
	}
	// Clip space to texture space. The y axis flips: clip space is y-up, the depth buffer y-down.
	// Clip space to texture space. The y axis flips: the draws use a y-flipped viewport, which puts clip
	// y = +1 at the top of the image, where v = 0. Measured: the other orientation gives 571 false
	// negatives against 45, so this is not a guess.
	uvMin = saturate(float2(ndcMin.x, -ndcMax.y) * 0.5 + 0.5);
	uvMax = saturate(float2(ndcMax.x, -ndcMin.y) * 0.5 + 0.5);
	nearestZ = minZ;
	return true;
}

// True when every depth under the object's screen extent is NEARER than the object's nearest point, which
// means the object is entirely behind what has already been drawn.
//
// The HZB holds the FARTHEST depth under each texel, so one value per corner of the extent at a mip whose
// texels are large enough that four of them cover it. Taking the maximum of those four and requiring it to
// be nearer than the object is conservative twice over: the mip is a max reduction, and the object is
// represented by the nearest point of a box that already contains its sphere.
bool Occluded(float3 boundCentre, float boundRadius, bool nativeVisibleForSample)
{
	if (HzbIndex == 0 || HzbMips == 0)
		return false;
	float2 uvMin, uvMax;
	float nearestZ;
	if (!ScreenExtent(boundCentre, boundRadius, uvMin, uvMax, nearestZ))
		return false;
	if (nearestZ <= 0.0)
		return false;  // in front of the near plane: nothing can occlude it

	// Image texture space to HZB texture space, before anything is measured in HZB texels.
	const float2 scale = HzbUvScale();
	uvMin *= scale;
	uvMax *= scale;

	const uint2 baseSize = HzbBaseSize();
	const float2 extent = (uvMax - uvMin) * float2(baseSize);
	// A mip whose texels are at least half the extent, so the four corner taps cover the whole rectangle.
	int mip = (int)ceil(log2(max(max(extent.x, extent.y), 1.0)));
	mip = clamp(mip, 0, (int)HzbMips - 1);
	const int2 mipSize = max(int2(baseSize) >> mip, int2(1, 1));
	const int2 texelMin = clamp(int2(uvMin * float2(mipSize)), int2(0, 0), mipSize - 1);
	const int2 texelMax = clamp(int2(uvMax * float2(mipSize)), int2(0, 0), mipSize - 1);

	Texture2D<float> hzb = ResourceDescriptorHeap[HzbIndex];
	const float farthest = max(
		max(hzb.Load(int3(texelMin.x, texelMin.y, mip)), hzb.Load(int3(texelMax.x, texelMin.y, mip))),
		max(hzb.Load(int3(texelMin.x, texelMax.y, mip)), hzb.Load(int3(texelMax.x, texelMax.y, mip))));

	RWByteAddressBuffer counters = ResourceDescriptorHeap[CountIndex];
	uint scratch;
	counters.InterlockedAdd(kCountHzbSampled, 1, scratch);
	if (farthest <= 1e-6)
		counters.InterlockedAdd(kCountHzbNear, 1, scratch);
	else if (farthest >= 0.9999)
		counters.InterlockedAdd(kCountHzbFar, 1, scratch);

	// A margin, because the two sides of this comparison are not in the same space. nearestZ is raw clip
	// space, while the HZB holds what was actually stored, which the viewport depth range has scaled - and
	// the native depth pass and DCLF's own draws do not even use the same range ([0, 0.999968] against
	// [0, 0.999998]). The gap is small but it is systematically in the direction that culls, and it bites
	// hardest on flat objects lying against the surface behind them, whose nearest corner is barely in
	// front of their own stored depth. Erring towards drawing is free; erring the other way loses objects.
	const float kDepthMargin = 1e-4;
	const bool occluded = farthest < nearestZ - kDepthMargin;

	// One sample of a rejection, so that a suspicious count can be read rather than guessed at: what the
	// HZB held, what the object's nearest corner was, where it was sampled and at which level. Written by
	// whichever thread gets there first; only the first rejection of the dispatch lands.
	if (occluded) {
		uint previous;
		counters.InterlockedCompareExchange(kSampleTaken, 0, 1, previous);
		if (previous == 0) {
			counters.Store(kSampleFarthest, asuint(farthest));
			counters.Store(kSampleNearestZ, asuint(nearestZ));
			counters.Store(kSampleUvMin, (uint(saturate(uvMin.x) * 65535.0) & 0xFFFF) | (uint(saturate(uvMin.y) * 65535.0) << 16));
			counters.Store(kSampleUvMax, (uint(saturate(uvMax.x) * 65535.0) & 0xFFFF) | (uint(saturate(uvMax.y) * 65535.0) << 16));
			counters.Store(kSampleMip, uint(mip) | (uint(nativeVisibleForSample) << 8));
		}
	}
	return occluded;
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	const uint draw = dispatchID.x;
	if (draw >= DrawCount)
		return;

	ByteAddressBuffer inputs = ResourceDescriptorHeap[InputsIndex];
	ByteAddressBuffer geometries = ResourceDescriptorHeap[GeometriesIndex];
	RWByteAddressBuffer sequences = ResourceDescriptorHeap[SequencesIndex];
	RWByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];
	RWByteAddressBuffer visibility = ResourceDescriptorHeap[VisibilityIndex];

	const uint inputOffset = draw * kInputStride;
	const uint4 input = inputs.Load4(inputOffset);  // pipeline index, record index, geometry index, flags
	const uint objectIndex = inputs.Load(inputOffset + 32);
	const bool nativeVisible = (input.w & kObjectNativeVisible) != 0;
	const bool drawable = (input.w & kInputDrawable) != 0;
	const uint phase = CullPhase();
	uint scratch;

	// Phase 2 only revisits what phase 1 provisionally rejected, and the colour segment tests nothing at
	// all - it draws what the two phases decided. Reading the decision rather than repeating it is what
	// keeps the depth and colour passes drawing the same set; the colour pass tests depth EQUAL, so an
	// object it draws without matching depth is invisible, and one the depth pass writes without a colour
	// draw is a hole that the native pass can no longer fill.
	const uint published = visibility.Load(objectIndex * 4);
	const bool publishedThisFrame = (published >> 2) == VisibilityStamp;
	const uint verdict = publishedThisFrame ? (published & 3) : kVisibilityVisible;
	if (phase == kPhaseTwo && !(publishedThisFrame && verdict == kVisibilityOccludedRetest))
		return;
	if (phase == kPhaseColour && verdict != kVisibilityVisible)
		return;

	bool cullRejected = false, frustumRejected = false, occlusionRejected = false;
	if (phase != kPhaseColour && CullMode() != 0) {
		// Word 2 separates "the culling rejected nothing" from "the culling did not run".
		count.InterlockedAdd(kCountTested, 1, scratch);
		const float4 bound = asfloat(inputs.Load4(inputOffset + 16));  // centre (world), radius
		// Phase 2 has already had its frustum answer from phase 1 and only revisits occlusion.
		frustumRejected = phase != kPhaseTwo && Culled(bound.xyz, bound.w);
		if (frustumRejected) {
			count.InterlockedAdd(kCountCulled, 1, scratch);
		} else if (CullMode() >= 2 && Occluded(bound.xyz, bound.w, nativeVisible)) {
			occlusionRejected = true;
			count.InterlockedAdd(kCountOccluded, 1, scratch);
		}
		cullRejected = frustumRejected || occlusionRejected;
	}

	// The engine's own decision is a useful reference, but only for the frustum test, where the engine is
	// exact and the two should agree: rejecting something it kept is then a defect, and is counted as one.
	//
	// It is NOT a reference for occlusion. The engine's occlusion is planes, boxes and room/portal
	// visibility, all of which keep plenty of geometry that is in fact hidden - that is the whole reason
	// for testing against a depth pyramid. An object the engine kept and the HZB rejected is therefore the
	// expected win, not a defect, and is counted separately so the two can never be confused. Whether such
	// a rejection was correct is a question about visibility, which only a depth test can answer
	// (CS_DCLF_CULL_VALIDATE), not one the engine's opinion can settle.
	if (frustumRejected && nativeVisible)
		count.InterlockedAdd(kCountFalseNegative, 1, scratch);
	if (occlusionRejected && nativeVisible)
		count.InterlockedAdd(kCountOccludedVisible, 1, scratch);
	if (!cullRejected && !nativeVisible && phase != kPhaseColour)
		count.InterlockedAdd(kCountRescued, 1, scratch);

	// Publish the decision. Phase 1 writes one for every candidate, so the buffer is completely rewritten
	// each frame and nothing stale survives into the colour segment.
	if (phase == kPhaseOne || phase == kPhaseTwo) {
		const uint decided = frustumRejected ? kVisibilityRejectedFinal :
			(occlusionRejected ? (phase == kPhaseOne ? kVisibilityOccludedRetest : kVisibilityRejectedFinal) : kVisibilityVisible);
		visibility.Store(objectIndex * 4, (VisibilityStamp << 2) | decided);
		if (phase == kPhaseTwo && !occlusionRejected)
			count.InterlockedAdd(kCountRescuedByPhaseTwo, 1, scratch);
	}

	if (cullRejected)
		return;
	if (RequireNativeVisible() && !nativeVisible) {
		if (phase != kPhaseColour)
			count.InterlockedAdd(kCountEngineCulled, 1, scratch);
		// The gate is about what may be DRAWN, so it must not change the published visibility: the colour
		// segment applies the same gate to the same objects and would otherwise disagree with itself.
		return;
	}
	// Cull-only inputs carry no bindings record, so there is nothing to draw even though the object is
	// visible and its visibility has been published.
	if (!drawable)
		return;

	const uint geometryOffset = input.z * kGeometryStride;
	const uint4 vertexBuffer = geometries.Load4(geometryOffset);        // address lo, hi, size, stride
	const uint4 indexBuffer = geometries.Load4(geometryOffset + 16);    // address lo, hi, size, index count
	const uint firstIndex = geometries.Load(geometryOffset + 32);

	// 64-bit record address = RecordsAddress + record index * RecordStride.
	const uint recordOffset = input.y * RecordStride;
	const uint recordLo = RecordsAddressLo + recordOffset;
	const uint recordHi = RecordsAddressHi + (recordLo < RecordsAddressLo ? 1 : 0);

	// Phase 2 appends into a reserved part of the same buffer, with a counter of its own, because its draw
	// is recorded separately and the offset a recorded draw starts at has to be known on the CPU.
	uint slot;
	count.InterlockedAdd(phase == kPhaseTwo ? kCountDrawnPhaseTwo : kCountDrawn, 1, slot);
	const uint base = (slot + (phase == kPhaseTwo ? kPhaseTwoSequenceBase : 0)) * kSequenceStride;
	sequences.Store(base + 0, input.x);
	sequences.Store2(base + 4, uint2(recordLo, recordHi));
	sequences.Store4(base + 12, vertexBuffer);
	sequences.Store4(base + 28, uint4(indexBuffer.xyz, kIndexFormatR16));
	sequences.Store4(base + 44, uint4(indexBuffer.w, 1, firstIndex, 0));
	sequences.Store(base + 60, 0);
}

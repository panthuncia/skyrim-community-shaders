#ifndef DCLF_PORTAL_PROGRAMS_HLSLI
#define DCLF_PORTAL_PROGRAMS_HLSLI

// The main camera's portal and occluder programs, and the test Process1 (AE 0x140e28390) makes with them before a node's OnVisible.
// It ends in BSCompoundFrustum::Process (0x140e320b0). PortalViews::Visible is the same test on the CPU, on the same words.

// AMD runs the uniform scan. On an RX 6600 (driver 2.0.353) the short-circuit interpreter below culled about 900 visible objects
// a frame. The uniform scan visits every record and agrees with the CPU.
#if defined(CS_GPU_AMD) && !defined(DCLF_PORTAL_UNIFORM_SCAN)
#	define DCLF_PORTAL_UNIFORM_SCAN 1
#endif

static const uint kPortalSetBytes = 112;
static const uint kPortalHeaderBytes = kPortalSetBytes + 16;
static const uint kPortalDirectoryBytes = 32;
static const uint kPortalOpBytes = 16;
static const uint kPortalNoProgram = 0xFFFFFFFFu;
static const uint kPortalReached = 1u << 0;
static const uint kPortalSkipView = 1u << 1;
// BuildDrawsLatch::portalProgramsIndex, the depth segment's published programs.
static const uint kLatchPortalPrograms = 320;

// DCLF_PORTAL_TRACE records how PortalTestAt decided, for PortalCapture's check against the CPU. x packs the verdict, deciding step,
// cull mode, view result, rejecting plane and frame stamp. y and z hash what was loaded. w is the rejecting plane's distance.
#ifdef DCLF_PORTAL_TRACE
static uint4 PortalTrace;
uint PortalTraceHash(uint a_hash, uint a_word)
{
	return (a_hash ^ a_word) * 16777619u;
}
#	define PORTAL_TRACE_STEP(a_step) PortalTrace.x = (PortalTrace.x & ~(15u << 8)) | ((a_step) << 8)
#else
#	define PORTAL_TRACE_STEP(a_step)
#endif

// Plane distance in the engine's order, y then x then z, less the constant.
float PortalDistance(float4 a_plane, float3 a_point)
{
	return ((a_plane.y * a_point.y + a_plane.x * a_point.x) + a_point.z * a_plane.z) - a_plane.w;
}

// The sphere test on the view planes (FUN_140d3ff10). Outside when the bound is wholly behind an active plane.
bool PortalViewTest(ByteAddressBuffer a_programs, uint a_base, float3 a_centre, float a_radius)
{
	const uint mask = a_programs.Load(a_base + 96u);
#ifdef DCLF_PORTAL_TRACE
	// The same test with every plane loaded and hashed, keeping the first rejecting plane.
	uint hash = 2166136261u, rejected = 15u;
	float nearest = 3.4e38f;
	[unroll] for (uint p = 0; p < 6u; ++p)
	{
		const uint4 words = a_programs.Load4(a_base + p * 16u);
		hash = PortalTraceHash(PortalTraceHash(PortalTraceHash(PortalTraceHash(hash, words.x), words.y), words.z), words.w);
		const float d = PortalDistance(asfloat(words), a_centre);
		if ((mask & (1u << p)) != 0u && rejected == 15u) {
			if (d <= -a_radius) {
				rejected = p;
				nearest = d;
			} else
				nearest = min(nearest, d);
		}
	}
	PortalTrace.y = PortalTraceHash(hash, mask);
	PortalTrace.x |= (rejected == 15u ? 1u << 16 : 0u) | (rejected << 20);
	PortalTrace.w = asuint(nearest);
	return rejected == 15u;
#else
	[unroll] for (uint p = 0; p < 6u; ++p)
	{
		if ((mask & (1u << p)) != 0u && PortalDistance(asfloat(a_programs.Load4(a_base + p * 16u)), a_centre) <= -a_radius)
			return false;
	}
	return true;
#endif
}

// Both bytecode operations share one fixed six-plane calculation. No per-plane early exits.
bool PortalPlaneSetTest(ByteAddressBuffer a_programs, uint a_base, uint a_kind, float3 a_centre, float a_radius)
{
	const uint mask = a_programs.Load(a_base + 96u);
	bool outside = false;
	bool notContained = false;
	[unroll] for (uint p = 0; p < 6u; ++p)
	{
		const bool active = (mask & (1u << p)) != 0u;
		const precise float d = PortalDistance(asfloat(a_programs.Load4(a_base + p * 16u)), a_centre);
		outside = outside || (active && d <= -a_radius);
		notContained = notContained || (active && (d <= -a_radius || d < a_radius));
	}
	return a_kind == 7u ? !outside : notContained;
}

// Forward-only compact instructions {kind, set, true target, false target}, validated on the CPU.
// The uniform scan visits every record in order. The default follows each lane's short-circuit path.
bool PortalProgramRun(ByteAddressBuffer a_programs, uint4 a_entry, uint4 a_entry2, uint a_recordsBase, uint a_setsBase, float3 a_centre, float a_radius)
{
	const uint records = a_recordsBase + a_entry2.x * kPortalOpBytes;
	const uint sets = a_setsBase + a_entry2.y * kPortalSetBytes;
	uint op = a_entry.w;
	// The loops never run past the records the encoding holds, so a torn buffer gives one wrong frame, never a GPU hang.
	// Written with min rather than a guard and early return, because of AMD's equality-guard miscompile.
	// A sets base below the records base (stale words) gives none rather than an unsigned wrap. 4096 is the CPU encoder's own guard.
	const uint available = (max(a_setsBase, a_recordsBase) - a_recordsBase) / kPortalOpBytes;
	const uint operators = min(min(a_entry.y, available - min(a_entry2.x, available)), 4096u);
#ifdef DCLF_PORTAL_UNIFORM_SCAN
	bool accepted = false;
	[loop] for (uint i = 0; i < operators; ++i)
	{
		const uint4 record = a_programs.Load4(records + i * kPortalOpBytes);
		if (record.x == 2u || record.x == 3u) {
			accepted = accepted || (op == i && record.x == 2u);
		} else {
			const bool result = PortalPlaneSetTest(a_programs, sets + record.y * kPortalSetBytes, record.x, a_centre, a_radius);
			op = op == i ? (result ? record.z : record.w) : op;
		}
	}
	return accepted;
#else
	// A forward-only path visits each record at most once.
	[loop] for (uint step = 0; step < min(a_entry2.w, operators); ++step)
	{
		// A producer invariant failure must never silently accept a bound.
		if (op >= operators)
			return false;
		const uint4 record = a_programs.Load4(records + op * kPortalOpBytes);
		// Ops 2 and 3 end the program. Range compares keep these guards free of equality tests.
		if (record.x - 2u < 2u)
			return record.x == 2u;
		if (record.x - 7u >= 2u || record.y >= a_entry.z)
			return false;
		const bool result = PortalPlaneSetTest(a_programs, sets + record.y * kPortalSetBytes, record.x, a_centre, a_radius);
		op = result ? record.z : record.w;
	}
	return false;
#endif
}

// The interpreter, or with DCLF_PORTAL_EXPERIMENTAL a PortalEvaluators.hlsli evaluator for the portal bench.
#ifdef DCLF_PORTAL_EXPERIMENTAL
#	include "DrawcallLimitFix/PortalEvaluators.hlsli"
#else
bool PortalProgramEvaluate(ByteAddressBuffer a_programs, uint a_base, uint a_program, uint4 a_entry, uint4 a_entry2, uint a_recordsBase, uint a_setsBase, float3 a_centre, float a_radius)
{
	return PortalProgramRun(a_programs, a_entry, a_entry2, a_recordsBase, a_setsBase, a_centre, a_radius);
}
#endif

// 1 in view, 0 culled, -1 no program. The flag arguments are the node's flags 11, 12 and 20.
// The trace's steps, in PortalTrace.x bits 8-11.
static const uint kPortalTraceNoProgram = 1;
static const uint kPortalTraceNoRadius = 2;
static const uint kPortalTraceUnreached = 3;
static const uint kPortalTraceCullMode = 4;      // cull mode 1 or 2, or always drawn
static const uint kPortalTracePreprocessed = 5;
static const uint kPortalTraceViewOnly = 6;      // cull mode 3, or no operators
static const uint kPortalTraceViewRejected = 7;  // ahead of the operators
static const uint kPortalTraceOperators = 8;

int PortalTestAtSteps(ByteAddressBuffer a_programs, uint a_base, uint a_program, float3 a_centre, float a_radius, bool a_alwaysDraw, bool a_preprocessed, bool a_preprocessHidden)
{
	const uint count = a_programs.Load(a_base + kPortalSetBytes);
	PORTAL_TRACE_STEP(kPortalTraceNoProgram);
	// One unsigned test also covers kPortalNoProgram. On the RX 6600 (driver 26.10.1) "a_program == kPortalNoProgram || a_program >= count"
	// was miscompiled. Past the return it took a_program as kPortalNoProgram and skipped the room test.
	if (a_program >= count)
		return -1;
	const uint directory = a_base + kPortalHeaderBytes + a_program * kPortalDirectoryBytes;
	const uint4 entry = a_programs.Load4(directory);         // flags, operators, sets, first operator
	const uint4 entry2 = a_programs.Load4(directory + 16u);  // first record, first set, cull mode, maximum path length
#ifdef DCLF_PORTAL_TRACE
	uint hash = PortalTraceHash(2166136261u, count);
	hash = PortalTraceHash(PortalTraceHash(PortalTraceHash(PortalTraceHash(hash, entry.x), entry.y), entry.z), entry.w);
	PortalTrace.z = PortalTraceHash(PortalTraceHash(PortalTraceHash(PortalTraceHash(hash, entry2.x), entry2.y), entry2.z), entry2.w);
	PortalTrace.x |= min(entry2.z, 15u) << 12;
#endif
	PORTAL_TRACE_STEP(kPortalTraceNoRadius);
	if (a_radius == 0.0f && !a_alwaysDraw)
		return 0;
	// A room the walk did not reach is not culled into.
	PORTAL_TRACE_STEP(kPortalTraceUnreached);
	if (a_program != 0u && (entry.x & kPortalReached) == 0u)
		return 0;
	const uint cullMode = entry2.z;
	PORTAL_TRACE_STEP(kPortalTraceCullMode);
	if (cullMode == 2u)
		return 0;
	const uint drawMode = a_alwaysDraw ? 1u : cullMode;
	if (drawMode == 1u)
		return 1;
	PORTAL_TRACE_STEP(kPortalTracePreprocessed);
	if (a_preprocessed && cullMode != 4u)
		return a_preprocessHidden ? 0 : 1;
	const bool view = PortalViewTest(a_programs, a_base, a_centre, a_radius);
	PORTAL_TRACE_STEP(kPortalTraceViewOnly);
	const uint operators = cullMode == 3u ? 0u : entry.y;
	if (operators == 0u)
		return view ? 1 : 0;
	PORTAL_TRACE_STEP(kPortalTraceViewRejected);
	if ((entry.x & kPortalSkipView) == 0u && !view)
		return 0;
	PORTAL_TRACE_STEP(kPortalTraceOperators);
	const uint recordsBase = a_base + kPortalHeaderBytes + count * kPortalDirectoryBytes;
	const uint setsBase = recordsBase + a_programs.Load(a_base + kPortalSetBytes + 4u) * kPortalOpBytes;
	return PortalProgramEvaluate(a_programs, a_base, a_program, entry, entry2, recordsBase, setsBase, a_centre, a_radius) ? 1 : 0;
}
#undef PORTAL_TRACE_STEP

int PortalTestAt(ByteAddressBuffer a_programs, uint a_base, uint a_program, float3 a_centre, float a_radius, bool a_alwaysDraw, bool a_preprocessed, bool a_preprocessHidden)
{
#ifdef DCLF_PORTAL_TRACE
	PortalTrace = uint4(0u, 0u, 0u, 0u);
	const int verdict = PortalTestAtSteps(a_programs, a_base, a_program, a_centre, a_radius, a_alwaysDraw, a_preprocessed, a_preprocessHidden);
	PortalTrace.x |= asuint(verdict) & 0xFFu;
	return verdict;
#else
	return PortalTestAtSteps(a_programs, a_base, a_program, a_centre, a_radius, a_alwaysDraw, a_preprocessed, a_preprocessHidden);
#endif
}

// The standalone main-camera buffer is the same encoding at byte offset zero.
int PortalTest(ByteAddressBuffer a_programs, uint a_program, float3 a_centre, float a_radius, bool a_alwaysDraw, bool a_preprocessed, bool a_preprocessHidden)
{
	return PortalTestAt(a_programs, 0u, a_program, a_centre, a_radius, a_alwaysDraw, a_preprocessed, a_preprocessHidden);
}

// A portal-strict point light's per-object gate. The object's gate, that gate's verdict for the light's context in the frame's atlas,
// and the bound in the verdict's room frustum. A publication of another atlas, or a context it lacks, gates nothing.
static const uint kStrictGateClosed = 0xFFFFFFFEu;
static const uint kStrictGateHeaderBytes = 32;
static const uint kContextEntryBytes = 64;

// Process1's compound test on a strict light's room program. A point process has no view planes.
bool StrictProgramPass(ByteAddressBuffer a_atlas, uint a_base, uint a_program, float3 a_centre, float a_radius)
{
	const uint count = a_atlas.Load(a_base + kPortalSetBytes);
	if (a_program >= count)
		return true;
	const uint directory = a_base + kPortalHeaderBytes + a_program * kPortalDirectoryBytes;
	const uint4 entry = a_atlas.Load4(directory);
	const uint4 entry2 = a_atlas.Load4(directory + 16u);
	if (entry.y == 0u)
		return true;
	const uint recordsBase = a_base + kPortalHeaderBytes + count * kPortalDirectoryBytes;
	const uint setsBase = recordsBase + a_atlas.Load(a_base + kPortalSetBytes + 4u) * kPortalOpBytes;
	return PortalProgramRun(a_atlas, entry, entry2, recordsBase, setsBase, a_centre, a_radius);
}

// Whether a strict light culls the object.
bool StrictGateRejects(uint a_gatesIndex, uint a_atlasIndex, uint a_objectGatesIndex, uint a_context, uint a_object, float3 a_centre, float a_radius)
{
	if (min(min(a_gatesIndex, a_atlasIndex), a_objectGatesIndex) == 0u)
		return false;
	ByteAddressBuffer gates = ResourceDescriptorHeap[a_gatesIndex];
	ByteAddressBuffer atlas = ResourceDescriptorHeap[a_atlasIndex];
	const uint4 head = gates.Load4(0);  // gates, contexts, atlas identity low, high
	const uint4 atlasHead = atlas.Load4(0);  // contexts, buckets, identity low, high
	// Another atlas's publication or no gates leaves no context to test.
	const uint contexts = all(head.zw == atlasHead.zw) && head.x != 0u ? min(head.y, atlasHead.x) : 0u;
	if (a_context >= contexts)
		return false;
	StructuredBuffer<uint> column = ResourceDescriptorHeap[a_objectGatesIndex];
	uint objects, stride;
	column.GetDimensions(objects, stride);
	uint gate = a_object < objects ? column[a_object] : 0u;
	gate = gate < head.x ? gate : 0u;
	const uint verdict = gates.Load(kStrictGateHeaderBytes + (gate * head.y + a_context) * 4u);
	if (verdict == kStrictGateClosed)
		return true;
	if (verdict == kPortalNoProgram)
		return false;
	// ContextEntry::programsBase, absolute bytes into the atlas.
	const uint base = atlas.Load(16u + a_context * kContextEntryBytes + 24u);
	return !StrictProgramPass(atlas, base, verdict, a_centre, a_radius);
}

#endif

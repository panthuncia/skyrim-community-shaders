#include "DrawcallLimitFix/PortalContexts.hlsli"
#include "DrawcallLimitFix/PortalPrograms.hlsli"
#include "DrawcallLimitFix/PortalQueries.hlsli"

cbuffer PortalBatchConstants : register(b0)
{
	uint QueriesIndex;
	uint ResultsIndex;
	uint ProgramsIndex;
	uint TasksIndex;  // CS_DCLF_PORTAL_TRACE: each query's decision trace
	uint QueryCount;
	// The depth latch, whose portalProgramsIndex names the frame's programs when ProgramsIndex is 0. A context atlas batch binds its publication directly.
	uint LatchIndex;
	uint LatchOffset;
	// The portal bench's auxiliary table, 0 in production.
	uint AuxIndex;
	uint TraceFrame;  // CS_DCLF_PORTAL_TRACE, the scene frame the pass prepared for
	uint3 Padding;
}

ByteAddressBuffer Programs()
{
	uint index = ProgramsIndex;
	if (index == 0u && LatchIndex != 0u) {
		ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
		index = latch.Load(LatchOffset + kLatchPortalPrograms);
	}
	ByteAddressBuffer programs = ResourceDescriptorHeap[index];
	return programs;
}

// A query's verdict against its program: 1 in view, 0 culled, -1 no program, -2 an invalid atlas context.
int QueryVerdict(PortalQuery query)
{
#ifdef DCLF_PORTAL_EXPERIMENTAL
	PortalAuxIndex = AuxIndex;
#endif
	ByteAddressBuffer programs = Programs();
	uint programsBase = 0, program = query.Program;
#ifdef DCLF_PORTAL_CONTEXT_ATLAS
	uint contextFrame;
	if (!PortalContextProgram(programs, query.Program, programsBase, program, contextFrame) || contextFrame != query.Generation)
		return -2;
#endif
	return PortalTestAt(programs, programsBase, program, query.Bound.xyz, query.Bound.w,
		(query.Flags & (1u << 11)) != 0, (query.Flags & (1u << 12)) != 0, (query.Flags & (1u << 20)) != 0);
}

// One thread per query against its own program. DCLF_PORTAL_FLOOR (the portal bench) skips evaluation to measure memory cost alone.
[numthreads(64, 1, 1)] void Inline(uint3 id : SV_DispatchThreadID)
{
	if ((id.x + id.y * (64u * 65535u)) >= QueryCount)
		return;
	StructuredBuffer<PortalQuery> queries = ResourceDescriptorHeap[QueriesIndex];
	const PortalQuery query = queries[(id.x + id.y * (64u * 65535u))];
	RWStructuredBuffer<uint2> results = ResourceDescriptorHeap[ResultsIndex];
#ifdef DCLF_PORTAL_FLOOR
	results[query.Destination] = uint2(query.Generation, query.Program);
#else
	if ((query.Flags & 0x80000000u) != 0u) {
		results[query.Destination] = uint2(query.Generation, query.Program);
		return;
	}
	results[query.Destination] = uint2(query.Generation, asuint(QueryVerdict(query)));
#	ifdef DCLF_PORTAL_TRACE
	RWStructuredBuffer<uint4> trace = ResourceDescriptorHeap[TasksIndex];
	trace[(id.x + id.y * (64u * 65535u))] = uint4(PortalTrace.x | (TraceFrame << 24), PortalTrace.yzw);
#	endif
#endif
}

#ifndef DCLF_PORTAL_QUERIES_HLSLI
#define DCLF_PORTAL_QUERIES_HLSLI
// PortalContext.h's GPU ABI. Destinations and generations are stable.
struct PortalQuery
{
	float4 Bound;
	uint Program;
	uint Flags;
	uint Destination;
	uint Generation;
};
#endif

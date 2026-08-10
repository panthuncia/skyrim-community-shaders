#define ORG_RENDER_GRAPH_HOST_EXPORTS
#include <OpenRenderGraph/ContributorRuntime.h>

extern "C" ORGStatus ORG_RG_CALL ORG_GetRenderGraphAPI(
	uint32_t version, ORGRenderGraphAPI* out)
{
	auto* runtime = org::contributor::Runtime::GetCurrentForExport();
	return runtime ? runtime->GetAPI(version, out) : ORG_RG_E_RUNTIME_UNAVAILABLE;
}

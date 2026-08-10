#include <CommunityShaders/RenderGraphAPI.h>
#include <CommunityShaders/GPUServiceAPI.h>

#include <stddef.h>

_Static_assert(sizeof(CSRGRegistrationHandle) == 8, "registration handles are 64-bit");
_Static_assert(sizeof(CSRGBinding) == 4, "bindings are 32-bit");
_Static_assert(offsetof(CSRGResourceDesc, structSize) == 0, "ABI header must be first");
_Static_assert(offsetof(CSRGPassDesc, structSize) == 0, "ABI header must be first");
_Static_assert(offsetof(CSRGContributorDesc, structSize) == 0, "ABI header must be first");
_Static_assert(CS_RG_FORMAT_COUNT == 80, "format ABI changed; revise the version");

int main(void)
{
	CSRenderGraphAPI graph = { sizeof(graph), CS_RENDER_GRAPH_API_CURRENT };
	CSGPUServiceAPI services = { sizeof(services), CS_GPU_SERVICE_API_VERSION_1 };
	return graph.apiVersion == 2 && services.apiVersion == 1 ? 0 : 1;
}

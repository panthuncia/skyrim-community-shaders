#include <OpenRenderGraph/ContributorAPI.h>
#include <CommunityShaders/RenderGraphHost.h>
#include <OpenRenderGraph/ShaderCompilerService.h>

#include <stddef.h>

_Static_assert(sizeof(ORGRegistrationHandle) == 8, "registration handles are 64-bit");
_Static_assert(sizeof(ORGBinding) == 4, "bindings are 32-bit");
_Static_assert(offsetof(ORGResourceDesc, structSize) == 0, "ABI header must be first");
_Static_assert(offsetof(ORGPassDesc, structSize) == 0, "ABI header must be first");
_Static_assert(offsetof(ORGContributorDesc, structSize) == 0, "ABI header must be first");
_Static_assert(ORG_RG_FORMAT_COUNT == 80, "format ABI changed; revise the version");

int main(void)
{
	ORGRenderGraphAPI graph = { sizeof(graph), ORG_RENDER_GRAPH_API_CURRENT };
	ORGShaderCompilerServiceAPI services = { sizeof(services), ORG_SHADER_COMPILER_SERVICE_VERSION_1 };
	return graph.apiVersion == 1 && services.apiVersion == 1 ? 0 : 1;
}

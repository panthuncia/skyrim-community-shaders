#include <OpenRenderGraph/ContributorAPI.h>
#include <CommunityShaders/RenderGraphHost.h>
#include <Windows.h>

namespace
{
	ORGRenderGraphAPI api{ sizeof(api), ORG_RENDER_GRAPH_API_CURRENT };
	ORGRegistrationHandle registration{};

	ORGStatus ORG_RG_CALL Execute(void*, const ORGExecutionContext* context)
	{
		return context && context->frame ? ORG_RG_OK : ORG_RG_E_INVALID_ARGUMENT;
	}

	ORGStatus ORG_RG_CALL Build(void*, ORGBuildHandle build)
	{
		ORGResourceDesc resource{};
		resource.structSize = sizeof(resource);
		resource.apiVersion = ORG_RENDER_GRAPH_API_CURRENT;
		resource.id = "sample.producer.values";
		resource.lifetime = ORG_RG_RESOURCE_TRANSIENT;
		resource.dimension = ORG_RG_RESOURCE_BUFFER;
		resource.heapClass = ORG_RG_HEAP_DEVICE_LOCAL;
		resource.sizing = ORG_RG_SIZE_ABSOLUTE;
		resource.format = ORG_RG_FORMAT_UNKNOWN;
		resource.byteSize = 4096;
		resource.mipLevels = 1;
		resource.sampleCount = 1;
		resource.allowedUsages = ORG_RG_USAGE_UNORDERED_ACCESS | ORG_RG_USAGE_SHADER_RESOURCE;
		auto status = api.DeclareResource(build, &resource);
		if (status != ORG_RG_OK) return status;

		ORGResourceAccessDesc access{};
		access.structSize = sizeof(access);
		access.apiVersion = ORG_RENDER_GRAPH_API_CURRENT;
		access.resourceId = "sample.producer.values";
		access.binding = 7;
		access.access = ORG_RG_ACCESS_UNORDERED_ACCESS;
		access.range = { 0, UINT32_MAX, 0, UINT32_MAX };
		access.elementCount = UINT32_MAX;
		ORGPassDesc pass{};
		pass.structSize = sizeof(pass);
		pass.apiVersion = ORG_RENDER_GRAPH_API_CURRENT;
		pass.id = "sample.producer.populate";
		pass.kind = ORG_RG_PASS_COMPUTE;
		pass.queue = ORG_RG_QUEUE_AUTOMATIC;
		pass.flags = ORG_RG_PASS_PARALLEL_RECORDING_SAFE;
		pass.accesses = &access;
		pass.accessCount = 1;
		pass.execute = &Execute;
		return api.DeclarePass(build, &pass);
	}

	bool LoadAPI()
	{
		const auto module = GetModuleHandleW(L"CommunityShaders.dll");
		const auto get = module ? reinterpret_cast<decltype(&ORG_GetRenderGraphAPI)>(
			GetProcAddress(module, "ORG_GetRenderGraphAPI")) : nullptr;
		return get && get(ORG_RENDER_GRAPH_API_CURRENT, &api) == ORG_RG_OK;
	}
}

extern "C" __declspec(dllexport) bool CS_Producer_Register()
{
	if (!LoadAPI()) return false;
	ORGContributorDesc contributor{};
	contributor.structSize = sizeof(contributor);
	contributor.apiVersion = ORG_RENDER_GRAPH_API_CURRENT;
	contributor.id = "sample.producer";
	contributor.kind = ORG_RG_CONTRIBUTOR_REQUIRED;
	contributor.build = &Build;
	return api.RegisterContributor(&contributor, &registration) == ORG_RG_OK;
}

extern "C" __declspec(dllexport) bool CS_Producer_BeginUnregister()
{
	return registration && api.BeginUnregister(registration) == ORG_RG_OK;
}

extern "C" __declspec(dllexport) uint32_t CS_Producer_State()
{
	ORGRegistrationState state = ORG_RG_REGISTRATION_RETIRED;
	return api.GetRegistrationState && api.GetRegistrationState(registration, &state) == ORG_RG_OK ? state : UINT32_MAX;
}

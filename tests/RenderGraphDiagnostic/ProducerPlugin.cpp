#include <CommunityShaders/RenderGraphAPI.h>
#include <Windows.h>

namespace
{
	CSRenderGraphAPI api{ sizeof(api), CS_RENDER_GRAPH_API_CURRENT };
	CSRGRegistrationHandle registration{};

	CSRGStatus CS_RG_CALL Execute(void*, const CSRGExecutionContext* context)
	{
		return context && context->frame ? CS_RG_OK : CS_RG_E_INVALID_ARGUMENT;
	}

	CSRGStatus CS_RG_CALL Build(void*, CSRGBuildHandle build)
	{
		CSRGResourceDesc resource{};
		resource.structSize = sizeof(resource);
		resource.apiVersion = CS_RENDER_GRAPH_API_CURRENT;
		resource.id = "sample.producer.values";
		resource.lifetime = CS_RG_RESOURCE_TRANSIENT;
		resource.dimension = CS_RG_RESOURCE_BUFFER;
		resource.heapClass = CS_RG_HEAP_DEVICE_LOCAL;
		resource.sizing = CS_RG_SIZE_ABSOLUTE;
		resource.format = CS_RG_FORMAT_UNKNOWN;
		resource.byteSize = 4096;
		resource.mipLevels = 1;
		resource.sampleCount = 1;
		resource.allowedUsages = CS_RG_USAGE_UNORDERED_ACCESS | CS_RG_USAGE_SHADER_RESOURCE;
		auto status = api.DeclareResource(build, &resource);
		if (status != CS_RG_OK) return status;

		CSRGResourceAccessDesc access{};
		access.structSize = sizeof(access);
		access.apiVersion = CS_RENDER_GRAPH_API_CURRENT;
		access.resourceId = "sample.producer.values";
		access.binding = 7;
		access.access = CS_RG_ACCESS_UNORDERED_ACCESS;
		access.range = { 0, UINT32_MAX, 0, UINT32_MAX };
		access.elementCount = UINT32_MAX;
		CSRGPassDesc pass{};
		pass.structSize = sizeof(pass);
		pass.apiVersion = CS_RENDER_GRAPH_API_CURRENT;
		pass.id = "sample.producer.populate";
		pass.kind = CS_RG_PASS_COMPUTE;
		pass.queue = CS_RG_QUEUE_AUTOMATIC;
		pass.flags = CS_RG_PASS_PARALLEL_RECORDING_SAFE;
		pass.accesses = &access;
		pass.accessCount = 1;
		pass.execute = &Execute;
		return api.DeclarePass(build, &pass);
	}

	bool LoadAPI()
	{
		const auto module = GetModuleHandleW(L"CommunityShaders.dll");
		const auto get = module ? reinterpret_cast<decltype(&CS_GetRenderGraphAPI)>(
			GetProcAddress(module, "CS_GetRenderGraphAPI")) : nullptr;
		return get && get(CS_RENDER_GRAPH_API_CURRENT, &api) == CS_RG_OK;
	}
}

extern "C" __declspec(dllexport) bool CS_Producer_Register()
{
	if (!LoadAPI()) return false;
	CSRGContributorDesc contributor{};
	contributor.structSize = sizeof(contributor);
	contributor.apiVersion = CS_RENDER_GRAPH_API_CURRENT;
	contributor.id = "sample.producer";
	contributor.kind = CS_RG_CONTRIBUTOR_REQUIRED;
	contributor.build = &Build;
	return api.RegisterContributor(&contributor, &registration) == CS_RG_OK;
}

extern "C" __declspec(dllexport) bool CS_Producer_BeginUnregister()
{
	return registration && api.BeginUnregister(registration) == CS_RG_OK;
}

extern "C" __declspec(dllexport) uint32_t CS_Producer_State()
{
	CSRGRegistrationState state = CS_RG_REGISTRATION_RETIRED;
	return api.GetRegistrationState && api.GetRegistrationState(registration, &state) == CS_RG_OK ? state : UINT32_MAX;
}

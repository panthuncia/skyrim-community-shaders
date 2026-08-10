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
		const char* after[]{ "sample.producer.populate" };
		ORGResourceAccessDesc access{};
		access.structSize = sizeof(access);
		access.apiVersion = ORG_RENDER_GRAPH_API_CURRENT;
		access.resourceId = "sample.producer.values";
		access.binding = 19;
		access.access = ORG_RG_ACCESS_SHADER_RESOURCE;
		access.range = { 0, UINT32_MAX, 0, UINT32_MAX };
		access.elementCount = UINT32_MAX;
		ORGPassDesc pass{};
		pass.structSize = sizeof(pass);
		pass.apiVersion = ORG_RENDER_GRAPH_API_CURRENT;
		pass.id = "sample.consumer.read";
		pass.kind = ORG_RG_PASS_COMPUTE;
		pass.queue = ORG_RG_QUEUE_AUTOMATIC;
		pass.after = after;
		pass.afterCount = 1;
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

extern "C" __declspec(dllexport) bool CS_Consumer_Register()
{
	if (!LoadAPI()) return false;
	ORGContributorDesc contributor{};
	contributor.structSize = sizeof(contributor);
	contributor.apiVersion = ORG_RENDER_GRAPH_API_CURRENT;
	contributor.id = "sample.consumer";
	contributor.kind = ORG_RG_CONTRIBUTOR_REQUIRED;
	contributor.build = &Build;
	return api.RegisterContributor(&contributor, &registration) == ORG_RG_OK;
}

extern "C" __declspec(dllexport) bool CS_Consumer_BeginUnregister()
{
	return registration && api.BeginUnregister(registration) == ORG_RG_OK;
}

extern "C" __declspec(dllexport) uint32_t CS_Consumer_State()
{
	ORGRegistrationState state = ORG_RG_REGISTRATION_RETIRED;
	return api.GetRegistrationState && api.GetRegistrationState(registration, &state) == ORG_RG_OK ? state : UINT32_MAX;
}

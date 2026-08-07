#include <CommunityShaders/DX12GraphAPI.h>
#include <Windows.h>
#include <d3d12.h>

#include <atomic>

namespace
{
	CSDX12GraphAPI api{ sizeof(api), CS_DX12_GRAPH_API_VERSION_1 };
	CSDX12RegistrationHandle registration{};
	std::atomic_uint64_t executedFrame{ UINT64_MAX };

	CSDX12Status CS_DX12_GRAPH_CALL Execute(void*, void* borrowedList, const CSDX12FrameInfo* frame)
	{
		if (!borrowedList || !frame)
			return CS_DX12_E_INVALID_ARGUMENT;
		auto* list = static_cast<ID3D12GraphicsCommandList*>(borrowedList);
		list->SetMarker(0, "cs-diagnostic-frame-marker", 26);
		executedFrame.store(frame->frameIndex, std::memory_order_release);
		return CS_DX12_OK;
	}

	CSDX12Status CS_DX12_GRAPH_CALL Build(void*, CSDX12BuildHandle build)
	{
		const char* after[]{ "cs.gbuffer.ready" };
		const char* before[]{ "cs.deferred-lighting.begin" };
		CSDX12PassDesc pass{};
		pass.structSize = sizeof(pass);
		pass.apiVersion = CS_DX12_GRAPH_API_VERSION_1;
		pass.id = "cs-diagnostic.marker";
		pass.queue = CS_DX12_QUEUE_GRAPHICS;
		pass.after = after;
		pass.afterCount = 1;
		pass.before = before;
		pass.beforeCount = 1;
		pass.execute = &Execute;
		CSDX12PassHandle handle{};
		return api.DeclarePass(build, &pass, &handle);
	}
}

extern "C" __declspec(dllexport) bool CS_Diagnostic_Register()
{
	const auto cs = GetModuleHandleW(L"CommunityShaders.dll");
	if (!cs)
		return false;
	const auto getApi = reinterpret_cast<decltype(&CS_GetDX12GraphAPI)>(GetProcAddress(cs, "CS_GetDX12GraphAPI"));
	if (!getApi || getApi(CS_DX12_GRAPH_API_VERSION_1, &api) != CS_DX12_OK)
		return false;
	CSDX12ContributorDesc contributor{};
	contributor.structSize = sizeof(contributor);
	contributor.apiVersion = CS_DX12_GRAPH_API_VERSION_1;
	contributor.id = "cs-diagnostic";
	contributor.kind = CS_DX12_CONTRIBUTOR_DIAGNOSTIC;
	contributor.build = &Build;
	return api.RegisterContributor(&contributor, &registration) == CS_DX12_OK;
}

extern "C" __declspec(dllexport) uint64_t CS_Diagnostic_LastExecutedFrame()
{
	return executedFrame.load(std::memory_order_acquire);
}

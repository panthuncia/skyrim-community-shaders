#define CS_RENDER_GRAPH_EXPORTS
#include "RenderGraphRegistry.h"
#include "RenderGraphRuntime.h"
#include "RenderGraphExecutionContext.h"

namespace
{
	CSRGStatus CS_RG_CALL RuntimeInfo(CSRGRuntimeInfo* out)
	{
		if (!out || out->structSize < sizeof(*out)) return CS_RG_E_INVALID_ARGUMENT;
		auto& runtime = RenderGraphRuntime::Get();
		if (!runtime.IsAvailable()) return CS_RG_E_RUNTIME_UNAVAILABLE;
		uint64_t capabilities = CS_RG_CAP_RENDER_PASSES | CS_RG_CAP_COMPUTE_PASSES | CS_RG_CAP_COPY_PASSES |
			CS_RG_CAP_ASYNC_COMPUTE | CS_RG_CAP_COPY_QUEUE | CS_RG_CAP_MANAGED_RESOURCES |
			CS_RG_CAP_RESOURCE_ALIASING;
#if defined(CS_HAS_ORG_MODULE_SERVICES)
		capabilities |= CS_RG_CAP_GPU_SERVICES;
#endif
		*out = { sizeof(*out), CS_RENDER_GRAPH_API_CURRENT, 1u, CS_RG_BACKEND_D3D12,
			capabilities, runtime.GetActiveGeneration(), runtime.GetLastSubmittedCompletion(),
			runtime.GetCompletedGraphValue(), runtime.GetFramesInFlight(), 0 };
		return CS_RG_OK;
	}
	CSRGStatus CS_RG_CALL Register(const CSRGContributorDesc* d, CSRGRegistrationHandle* h) {
		const auto status = RenderGraphRegistry::Get().Register(d, h);
		if (status == CS_RG_OK) RenderGraphRuntime::Get().RequestGraphRebuild();
		return status;
	}
	CSRGStatus CS_RG_CALL Unregister(CSRGRegistrationHandle h) {
		const auto status = RenderGraphRegistry::Get().BeginUnregister(h);
		if (status == CS_RG_OK) RenderGraphRuntime::Get().RequestGraphRebuild();
		return status;
	}
	CSRGStatus CS_RG_CALL RegistrationState(CSRGRegistrationHandle h, CSRGRegistrationState* s) { return RenderGraphRegistry::Get().GetRegistrationState(h, s); }
	CSRGStatus CS_RG_CALL Resource(CSRGBuildHandle h, const CSRGResourceDesc* d) { return RenderGraphRegistry::Get().DeclareResource(h, d); }
	CSRGStatus CS_RG_CALL Pass(CSRGBuildHandle h, const CSRGPassDesc* d) { return RenderGraphRegistry::Get().DeclarePass(h, d); }
	CSRGStatus CS_RG_CALL Rebuild(CSRGRegistrationHandle h) {
		const auto status = RenderGraphRegistry::Get().RequestRebuild(h);
		if (status == CS_RG_OK) RenderGraphRuntime::Get().RequestGraphRebuild();
		return status;
	}
	CSRGStatus CS_RG_CALL Diagnostic(CSRGRegistrationHandle h, CSRGDiagnostic* d) { return RenderGraphRegistry::Get().GetDiagnostic(h, d); }
	CSRGStatus CS_RG_CALL BindingInfo(const CSRGExecutionContext* context, CSRGBinding binding, CSRGBindingInfo* out) {
		if (!context || !context->hostContext || !out || out->structSize < sizeof(*out)) return CS_RG_E_INVALID_ARGUMENT;
		const auto* host = static_cast<const RenderGraphExecutionContextHost*>(context->hostContext);
		if (host->magic != RenderGraphExecutionContextHost::kMagic) return CS_RG_E_INVALID_ARGUMENT;
		const auto it = host->bindingInfo.find(binding);
		if (it == host->bindingInfo.end()) return CS_RG_E_STALE_HANDLE;
		*out = it->second; return CS_RG_OK;
	}
	const char* CS_RG_CALL Status(CSRGStatus status) {
		switch (status) {
		case CS_RG_OK: return "success"; case CS_RG_E_INVALID_ARGUMENT: return "invalid argument";
		case CS_RG_E_UNSUPPORTED_VERSION: return "unsupported version"; case CS_RG_E_RUNTIME_UNAVAILABLE: return "runtime unavailable";
		case CS_RG_E_STALE_HANDLE: return "stale handle"; case CS_RG_E_DUPLICATE_ID: return "duplicate identifier";
		case CS_RG_E_RESERVED_ID: return "reserved identifier"; case CS_RG_E_MISSING_DEPENDENCY: return "missing dependency";
		case CS_RG_E_CYCLE: return "dependency cycle"; case CS_RG_E_INCOMPATIBLE_RESOURCE: return "incompatible resource";
		case CS_RG_E_WRONG_THREAD: return "wrong thread"; case CS_RG_E_CALLBACK_FAILED: return "callback failed";
		case CS_RG_E_CLOSED: return "closed"; case CS_RG_E_NOT_READY: return "not ready";
		case CS_RG_E_UNSUPPORTED_CAPABILITY: return "unsupported capability"; case CS_RG_E_DUPLICATE_BINDING: return "duplicate binding";
		default: return "internal error";
		}
	}
}

extern "C" CSRGStatus CS_RG_CALL CS_GetRenderGraphAPI(uint32_t version, CSRenderGraphAPI* out)
{
	if (!out || out->structSize < sizeof(*out)) return CS_RG_E_INVALID_ARGUMENT;
	if (version != CS_RENDER_GRAPH_API_CURRENT) return CS_RG_E_UNSUPPORTED_VERSION;
	*out = { sizeof(*out), version, &RuntimeInfo, &Register, &Unregister, &RegistrationState,
		&Resource, &Pass, &Rebuild, &Diagnostic, &BindingInfo, &Status };
	return CS_RG_OK;
}

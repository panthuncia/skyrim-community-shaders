#include "RenderGraphRuntime.h"

#include <CommunityShaders/RenderGraphHost.h>
#include <OpenRenderGraph/ContributorRuntime.h>
#include <OpenRenderGraph/ShaderCompilerService.h>

#include "Features/DeferredRendering/LightCulling.h"
#include "Features/DeferredRendering/DeferredShading.h"
#include "Features/DeferredRendering.h"

#include <cstdlib>

namespace {
	constexpr ORGAnchorDescriptor kAnchors[]{
		{ sizeof(ORGAnchorDescriptor), ORG_RENDER_GRAPH_API_CURRENT, CS_RG_ANCHOR_FRAME_BEGIN, "Frame begin" },
		{ sizeof(ORGAnchorDescriptor), ORG_RENDER_GRAPH_API_CURRENT, CS_RG_ANCHOR_SHADOWS_READY, "Shadows ready" },
		{ sizeof(ORGAnchorDescriptor), ORG_RENDER_GRAPH_API_CURRENT, CS_RG_ANCHOR_GBUFFER_READY, "G-buffer ready" },
		{ sizeof(ORGAnchorDescriptor), ORG_RENDER_GRAPH_API_CURRENT, CS_RG_ANCHOR_DEFERRED_LIGHTING_BEGIN, "Deferred lighting begin" },
		{ sizeof(ORGAnchorDescriptor), ORG_RENDER_GRAPH_API_CURRENT, CS_RG_ANCHOR_DEFERRED_LIGHTING_END, "Deferred lighting end" },
		{ sizeof(ORGAnchorDescriptor), ORG_RENDER_GRAPH_API_CURRENT, CS_RG_ANCHOR_FRAME_END, "Frame end" }
	};
}

RenderGraphRuntime& RenderGraphRuntime::Get()
{
	static RenderGraphRuntime instance;
	return instance;
}

RenderGraphRuntime::~RenderGraphRuntime() = default;

bool RenderGraphRuntime::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
	if (available.load(std::memory_order_acquire)) return true;
	try {
		if (!org::interop::D3D11Interop::CreateDeviceBundle(device, context, kCommandFrameCount,
			std::getenv("CS_DX12_DEBUG") != nullptr, deviceBundle)) {
			SetDiagnostic(ORG_RG_E_RUNTIME_UNAVAILABLE, "Cannot create D3D12 on Skyrim's D3D11 adapter");
			throw std::runtime_error("Cannot create D3D12 on Skyrim's D3D11 adapter");
		}
		graphicsQueue = deviceBundle.graphDevice->GetQueue(rhi::QueueKind::Graphics);
		if (!graphicsQueue || !CreateInterop())
			throw std::runtime_error("Required D3D11/D3D12 render-graph interop is unavailable");

		ORGHostDescriptor host{ sizeof(host), ORG_RENDER_GRAPH_API_CURRENT,
			CS_RENDER_GRAPH_HOST_ID, CS_RENDER_GRAPH_HOST_DISPLAY_NAME, CS_RENDER_GRAPH_HOST_VERSION,
			kAnchors, static_cast<uint32_t>(std::size(kAnchors)) };
		org::contributor::Runtime::RuntimeDesc desc{};
		desc.device = deviceBundle.graphDevice.Get();
		desc.backend = ORG_RG_BACKEND_D3D12;
		desc.framesInFlight = kCommandFrameCount;
		desc.host = host;
		desc.completedValue = [this] { return GetCompletedGraphValue(); };
		runtime = std::make_unique<org::contributor::Runtime>(std::move(desc));
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
		shaderCompiler = std::make_unique<org::services::ShaderCompiler>();
		shaderService = std::make_unique<org::services::ShaderServiceAdapter>(*shaderCompiler);
		runtime->RegisterService(ORG_SHADER_COMPILER_SERVICE_ID,
			[this](uint32_t version, void* table, uint32_t size) {
				return shaderService ? shaderService->Query(version, table, size) : ORG_RG_E_UNSUPPORTED_CAPABILITY;
			});
#endif
		renderThread = std::this_thread::get_id();
		available.store(true, std::memory_order_release);
		if (!DX12LightCulling::Get().Initialize(*this) || !DX12DeferredShading::Get().Initialize(*this)) {
			SetDiagnostic(ORG_RG_E_RUNTIME_UNAVAILABLE, "Failed to install CS native graph extensions");
			throw std::runtime_error("Failed to install required deferred graph extensions");
		}
		static std::once_flag shutdownRegistered;
		std::call_once(shutdownRegistered, [] { std::atexit([] { RenderGraphRuntime::Get().Shutdown(); }); });
		SetDiagnostic(ORG_RG_OK, "Render graph runtime initialized");
		return true;
	} catch (const std::exception& error) {
		SetDiagnostic(ORG_RG_E_INTERNAL, error.what());
		Shutdown();
		throw;
	} catch (...) {
		SetDiagnostic(ORG_RG_E_INTERNAL, "Exception while initializing render graph runtime");
		Shutdown();
		throw;
	}
}

bool RenderGraphRuntime::CreateInterop() noexcept
{
	interop = std::make_unique<org::interop::D3D11Interop>(deviceBundle.device11.get(), deviceBundle.graphDevice.Get());
	auto createPair = [&](rhi::TimelinePtr& timeline, winrt::com_ptr<ID3D11Fence>& fence, const char* name) {
		return rhi::IsOk(deviceBundle.graphDevice->CreateTimeline(timeline, 0, name, true)) && timeline &&
			interop->OpenTimeline(timeline.Get(), fence.put());
	};
	if (!createPair(readyTimeline, readyFence11, "CS D3D11 to ORG ready") ||
		!createPair(completeTimeline, completeFence11, "ORG to CS D3D11 complete") ||
		!interop->ProbeGraphOwnedSharing()) {
		SetDiagnostic(ORG_RG_E_RUNTIME_UNAVAILABLE, "D3D11/D3D12 sharing capability probe failed");
		return false;
	}
	return true;
}

bool RenderGraphRuntime::ExecuteGraph(uint32_t width, uint32_t height,
	uint32_t resourceWidth, uint32_t resourceHeight) noexcept
{
	if (!available.load(std::memory_order_acquire) || !runtime ||
		std::this_thread::get_id() != renderThread) return false;
	if (width != renderWidth || height != renderHeight || resourceWidth != allocationWidth || resourceHeight != allocationHeight) {
		renderWidth = width; renderHeight = height;
		allocationWidth = resourceWidth; allocationHeight = resourceHeight;
		runtime->RequestRebuild();
	}
	const auto ready = nextReadyFence.fetch_add(1, std::memory_order_relaxed);
	if (FAILED(deviceBundle.context11->Signal(readyFence11.get(), ready))) return false;
	const auto complete = nextCompleteFence.fetch_add(1, std::memory_order_relaxed);
	org::contributor::Runtime::FrameDesc frame{};
	frame.frameIndex = frameIndex++;
	frame.frameFenceValue = complete;
	frame.renderWidth = width; frame.renderHeight = height;
	frame.outputWidth = resourceWidth; frame.outputHeight = resourceHeight;
	frame.readyTimeline = readyTimeline.Get(); frame.readyValue = ready;
	frame.completeTimeline = completeTimeline.Get(); frame.completeValue = complete;
	if (!runtime->Execute(frame))
		return false;
	deviceBundle.context11->Wait(completeFence11.get(), complete);
	globals::features::deferredRendering.RetireFrames(GetCompletedGraphValue());
	return true;
}

void RenderGraphRuntime::Shutdown() noexcept
{
	available.store(false, std::memory_order_release);
	if (graphicsQueue && completeTimeline) {
		const auto value = nextCompleteFence.fetch_add(1, std::memory_order_relaxed);
		graphicsQueue.Signal({ completeTimeline->GetHandle(), value });
		completeTimeline->HostWait(value);
	}
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	if (runtime) runtime->UnregisterService(ORG_SHADER_COMPILER_SERVICE_ID);
	if (shaderService) shaderService->Shutdown();
#endif
	if (runtime) runtime->Shutdown();
	runtime.reset();
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	shaderService.reset(); shaderCompiler.reset();
#endif
	interop.reset();
	readyFence11 = nullptr; completeFence11 = nullptr;
	readyTimeline.Reset(); completeTimeline.Reset();
	graphicsQueue = {}; deviceBundle = {};
}

void RenderGraphRuntime::SetDiagnostic(int32_t status, std::string message) const noexcept
{
	std::scoped_lock lock(diagnosticMutex);
	const bool changed = diagnosticStatus != status || diagnosticMessage != message;
	diagnosticStatus = status;
	diagnosticMessage = std::move(message);
	if (status != ORG_RG_OK && changed)
		logger::error("[RenderGraphRuntime] Graph diagnostic {}: {}", status, diagnosticMessage);
}

void RenderGraphRuntime::RequestGraphRebuild() noexcept { if (runtime) runtime->RequestRebuild(); }
uint64_t RenderGraphRuntime::GetActiveGeneration() const noexcept { return runtime ? runtime->GetActiveGeneration() : 0; }
uint64_t RenderGraphRuntime::GetLastSubmittedCompletion() const noexcept
{ return runtime ? runtime->GetLastSubmittedCompletion() : 0; }

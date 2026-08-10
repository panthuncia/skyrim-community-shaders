#include "RenderGraphRuntime.h"
#include "RenderGraphRegistry.h"
#include "Features/DeferredRendering/LightCulling.h"
#include "Features/DeferredRendering/DeferredShading.h"
#include "Features/DeferredRendering.h"

#include <cstdlib>

struct RenderGraphRuntime::Generation
{
	uint64_t id{};
	RenderGraphRegistry::Candidate genericCandidate;
	uint64_t lastCompletion{};
};

RenderGraphRuntime& RenderGraphRuntime::Get()
{
	static RenderGraphRuntime instance;
	return instance;
}

bool RenderGraphRuntime::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
	if (available.load())
		return true;
	if (!device || !context)
		return false;
	try {
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(device11.put()))) || FAILED(context->QueryInterface(IID_PPV_ARGS(context11.put())))) {
			SetDiagnostic(CS_RG_E_RUNTIME_UNAVAILABLE, "D3D11.4 fence interfaces are unavailable");
			return false;
		}
		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		if (FAILED(device->QueryInterface(dxgiDevice.put())) || FAILED(dxgiDevice->GetAdapter(adapter.put()))) {
			SetDiagnostic(CS_RG_E_RUNTIME_UNAVAILABLE, "Cannot query the D3D11 device adapter");
			return false;
		}
		if (!CreateDeviceOnD3D11Adapter() || !CreateInterop())
			return false;
		renderThread = std::this_thread::get_id();
		available.store(true, std::memory_order_release);
		if (!DX12LightCulling::Get().Initialize(*this)) {
			available.store(false, std::memory_order_release);
			SetDiagnostic(CS_RG_E_RUNTIME_UNAVAILABLE, "Failed to initialize the CS clustered-lighting contributor");
			return false;
		}
		if (!DX12DeferredShading::Get().Initialize(*this)) {
			available.store(false, std::memory_order_release);
			SetDiagnostic(CS_RG_E_RUNTIME_UNAVAILABLE, "Failed to initialize the CS deferred-shading contributor");
			return false;
		}
		static std::once_flag shutdownRegistered;
		std::call_once(shutdownRegistered, [] { std::atexit([] { RenderGraphRuntime::Get().Shutdown(); }); });
		SetDiagnostic(CS_RG_OK, "Render graph runtime initialized on the D3D11 adapter");
		logger::info("[RenderGraphRuntime] BasicRHI device and graphics/compute/copy queues initialized on Skyrim's D3D11 adapter");
		logger::info("[RenderGraphRuntime] OpenRenderGraph host initialized and queue registry set up");
		return true;
	} catch (...) {
		SetDiagnostic(CS_RG_E_INTERNAL, "Exception while initializing render graph runtime");
		logger::error("[RenderGraphRuntime] Exception while initializing BasicRHI/OpenRenderGraph");
		return false;
	}
}

bool RenderGraphRuntime::CreateDeviceOnD3D11Adapter() noexcept
{
	rhi::DeviceCreateInfo createInfo{};
	createInfo.backend = rhi::Backend::D3D12;
	createInfo.framesInFlight = kCommandFrameCount;
	// Keep the debug layer opt-in: GPU validation is too expensive for normal play,
	// but the host must be diagnosable without requiring a special build.
	createInfo.enableDebug = std::getenv("CS_DX12_DEBUG") != nullptr;
	createInfo.nativeAdapter = adapter.get();
	if (rhi::Failed(rhi::CreateD3D12Device(createInfo, rhiDevice)) || !rhiDevice) {
		SetDiagnostic(CS_RG_E_RUNTIME_UNAVAILABLE, "BasicRHI failed to create D3D12 on the D3D11 adapter");
		return false;
	}
	rhiGraphicsQueue = rhiDevice->GetQueue(rhi::QueueKind::Graphics);
	rhiComputeQueue = rhiDevice->GetQueue(rhi::QueueKind::Compute);
	rhiCopyQueue = rhiDevice->GetQueue(rhi::QueueKind::Copy);
	if (!rhiGraphicsQueue || !rhiComputeQueue || !rhiCopyQueue) {
		SetDiagnostic(CS_RG_E_RUNTIME_UNAVAILABLE, "BasicRHI did not provide the required queues");
		return false;
	}
	renderGraph = RenderGraphHost::Create(rhiDevice.Get());
	return true;
}

bool RenderGraphRuntime::CreateInterop() noexcept
{
	interopCoordinator = std::make_unique<D3D11InteropBridge>(device11.get(), rhiDevice.Get());
	auto createPair = [&](rhi::TimelinePtr& timeline, winrt::com_ptr<ID3D11Fence>& fence11, const char* name) {
		if (rhi::Failed(rhiDevice->CreateTimeline(timeline, 0, name, true)) || !timeline)
			return false;
		return interopCoordinator->OpenTimeline(timeline.Get(), fence11.put());
	};
	if (!createPair(readyTimeline, readyFence11, "CS D3D11 to ORG ready") ||
		!createPair(completeTimeline, completeFence11, "ORG to CS D3D11 complete")) {
		SetDiagnostic(CS_RG_E_RUNTIME_UNAVAILABLE, "Failed to create D3D11/D3D12 shared timeline fences");
		return false;
	}
	if (!interopCoordinator->ProbeGraphOwnedSharing()) {
		SetDiagnostic(CS_RG_E_RUNTIME_UNAVAILABLE, "D3D12-owned cross-API Texture2D capability probe failed");
		interopCoordinator.reset();
		return false;
	}
	logger::info("[DX12Interop] D3D12-owned RGBA16 UAV/RT handoff capability probe succeeded");
	return true;
}

void RenderGraphRuntime::SetDiagnostic(CSRGStatus status, std::string message) const noexcept
{
	std::scoped_lock lock(diagnosticMutex);
	const bool changed = diagnosticStatus != status || diagnosticMessage != message;
	diagnosticStatus = status;
	++diagnosticSequence;
	diagnosticMessage = message;
	if (status != CS_RG_OK && changed)
		logger::error("[RenderGraphRuntime] Graph diagnostic {}: {}", static_cast<std::int32_t>(status), message);
}

bool RenderGraphRuntime::Rebuild() noexcept
{
	auto candidate = std::make_unique<Generation>();
	candidate->id = nextGeneration.fetch_add(1);
	const auto status = RenderGraphRegistry::Get().Compile(candidate->id, renderWidth, renderHeight,
		allocationWidth, allocationHeight, candidate->genericCandidate);
	if (status != CS_RG_OK) {
		SetDiagnostic(CS_RG_E_CALLBACK_FAILED, "Backend-neutral contributor candidate was rejected");
		return false;
	}
	try {
		renderGraph->SetStructuralDefinition(candidate->genericCandidate);
	} catch (const std::exception& error) {
		SetDiagnostic(CS_RG_E_INTERNAL, std::string("ORG structural compile failed: ") + error.what());
		return false;
	}
	if (active) retired.emplace_back(active->lastCompletion, std::move(active));
	active = std::move(candidate);
	RenderGraphRegistry::Get().Activate(active->genericCandidate);
	SetDiagnostic(CS_RG_OK, "Activated graph generation " + std::to_string(active->id));
	return true;
}

bool RenderGraphRuntime::ExecuteGraph(uint32_t width, uint32_t height, uint32_t resourceWidth, uint32_t resourceHeight) noexcept
{
	static bool firstExecution = true;
	static bool firstAttempt = true;
	if (!available.load() || std::this_thread::get_id() != renderThread)
		return false;
	if (width != renderWidth || height != renderHeight || resourceWidth != allocationWidth || resourceHeight != allocationHeight) {
		renderWidth = width;
		renderHeight = height;
		allocationWidth = resourceWidth;
		allocationHeight = resourceHeight;
		rebuildRequested.store(true);
	}
	if (firstAttempt) logger::info("[RenderGraphRuntime] Beginning first render-graph execution ({}x{})", width, height);
	if (rebuildRequested.exchange(false)) {
		if (firstAttempt) logger::info("[RenderGraphRuntime] Compiling first graph generation");
		if (!Rebuild()) {
			// Retain the active graph and retry the immutable candidate at the next boundary.
			rebuildRequested.store(true, std::memory_order_release);
		}
		if (firstAttempt) logger::info("[RenderGraphRuntime] First graph rebuild returned (active={})", active != nullptr);
		firstAttempt = false;
	}
	if (!active) {
		globals::features::deferredRendering.SetEnabledEvaluatorMask(0u);
		return false;
	}
	const uint64_t ready = nextReadyFence.fetch_add(1);
	if (FAILED(context11->Signal(readyFence11.get(), ready)))
		return false;
	if (firstExecution) logger::info("[RenderGraphRuntime] D3D11 readiness value {} queued", ready);
	const uint64_t complete = nextCompleteFence.fetch_add(1);
	RenderGraphHost::FrameInfo frame{ frameIndex, active->id, complete,
		static_cast<uint32_t>(frameIndex % kCommandFrameCount), kCommandFrameCount, width, height };
	try {
		if (firstExecution) logger::info("[RenderGraphRuntime] Submitting first ORG execution");
		renderGraph->Execute(static_cast<uint32_t>(frameIndex), complete, readyTimeline.Get(), ready,
			completeTimeline.Get(), complete, frame);
		if (firstExecution) logger::info("[RenderGraphRuntime] First ORG execution submitted");
	} catch (const std::exception& e) {
		logger::error("[RenderGraphRuntime] ORG execution failed: {}", e.what());
		if (rhiDevice) rhiDevice->CheckDebugMessages();
		if (rhi::g_breakCallback) rhi::g_breakCallback();
		SetDiagnostic(CS_RG_E_INTERNAL, std::string("ORG execution failed: ") + e.what());
		RenderGraphRegistry::Get().NotifyDeviceLost(0xffffffffu);
		available.store(false);
		logger::error("[RenderGraphRuntime] Graph recording failed; disabling further submissions and preserving the D3D11 path");
		globals::features::deferredRendering.SetEnabledEvaluatorMask(0u);
		return false;
	}
	++frameIndex;
	active->lastCompletion = complete;
	lastSubmittedCompletion.store(complete, std::memory_order_release);
	context11->Wait(completeFence11.get(), complete);
	if (firstExecution) logger::info("[RenderGraphRuntime] D3D11 completion wait {} queued", complete);
	firstExecution = false;
	RetireCompleted();
	return true;
}

void RenderGraphRuntime::RetireCompleted() noexcept
{
	const auto completed = completeTimeline ? completeTimeline->GetCompletedValue() : 0;
	if (renderGraph) renderGraph->Retire(completed);
	globals::features::deferredRendering.RetireFrames(completed);
	while (!retired.empty() && retired.front().first <= completed) {
		RenderGraphRegistry::Get().Retire(retired.front().second->id);
		retired.pop_front();
	}
}

void RenderGraphRuntime::Shutdown() noexcept
{
	available.store(false);
	if (rhiGraphicsQueue && completeTimeline) {
		const auto value = nextCompleteFence.fetch_add(1);
		rhiGraphicsQueue.Signal({ completeTimeline->GetHandle(), value });
		// Shutdown is the exceptional path where a CPU wait is required: graph
		// generations and callback code must remain alive until every queue has
		// passed the final graphics fan-in signal.
		completeTimeline->HostWait(value);
	}
	if (renderGraph) renderGraph->Retire(UINT64_MAX);
	RenderGraphRegistry::Get().Shutdown();
	std::unique_lock lock(registryMutex);
	active.reset(); retired.clear();
	renderGraph.reset();
	interopCoordinator.reset();
	readyFence11 = nullptr;
	completeFence11 = nullptr;
	readyTimeline.Reset(); completeTimeline.Reset();
	rhiGraphicsQueue = {}; rhiComputeQueue = {}; rhiCopyQueue = {}; rhiDevice.Reset();
	adapter = nullptr; context11 = nullptr; device11 = nullptr;
}

uint64_t RenderGraphRuntime::GetActiveGeneration() const noexcept
{
	std::shared_lock lock(registryMutex);
	return active ? active->id : 0;
}

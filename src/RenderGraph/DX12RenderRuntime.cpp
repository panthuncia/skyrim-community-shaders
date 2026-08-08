#define CS_DX12_GRAPH_EXPORTS
#include "DX12RenderRuntime.h"
#include "Features/DeferredRendering/LightCulling.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <unordered_set>
#include <rhi_interop_dx12.h>

namespace
{
	constexpr std::string_view kAnchors[] = {
		"cs.frame.begin", "cs.shadows.ready", "cs.gbuffer.ready",
		"cs.deferred-lighting.begin", "cs.deferred-lighting.end", "cs.frame.end"
	};

	bool ValidHeader(uint32_t size, uint32_t version, uint32_t required) noexcept
	{
		return size >= required && version == CS_DX12_GRAPH_API_CURRENT;
	}
}

struct DX12RenderRuntime::Contributor
{
	uint64_t handle{};
	std::string id;
	CSDX12ContributorKind kind{};
	void* userData{};
	CSDX12BuildCallback build{};
	CSDX12GenerationCallback generationActivated{};
	CSDX12GenerationCallback generationRetired{};
	CSDX12DeviceLostCallback deviceLost{};
	CSDX12ShutdownCallback shutdown{};
	bool unregisterRequested{};
};

struct DX12RenderRuntime::Resource
{
	uint64_t handle{};
	std::string id;
	std::string owner;
	CSDX12ResourceDesc desc{};
};

struct DX12RenderRuntime::Pass
{
	uint64_t handle{};
	std::string id;
	std::string owner;
	CSDX12QueuePolicy queuePolicy{};
	uint32_t flags{};
	std::vector<std::string> after;
	std::vector<std::string> before;
	std::vector<CSDX12ResourceAccessDesc> accesses;
	CSDX12ExecuteCallback execute{};
	void* userData{};
};

struct DX12RenderRuntime::Generation
{
	uint64_t id{};
	std::vector<Resource> resources;
	std::vector<Pass> passes;
	std::vector<Contributor> contributorSnapshot;
	uint64_t lastCompletion{};
};

struct DX12RenderRuntime::BuildState
{
	uint64_t handle{};
	const Contributor* contributor{};
	Generation* candidate{};
};

DX12RenderRuntime& DX12RenderRuntime::Get()
{
	static DX12RenderRuntime instance;
	return instance;
}

bool DX12RenderRuntime::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
	if (available.load())
		return true;
	if (!device || !context)
		return false;
	try {
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(device11.put()))) || FAILED(context->QueryInterface(IID_PPV_ARGS(context11.put())))) {
			SetDiagnostic(CS_DX12_E_RUNTIME_UNAVAILABLE, "D3D11.4 fence interfaces are unavailable");
			return false;
		}
		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		if (FAILED(device->QueryInterface(dxgiDevice.put())) || FAILED(dxgiDevice->GetAdapter(adapter.put()))) {
			SetDiagnostic(CS_DX12_E_RUNTIME_UNAVAILABLE, "Cannot query the D3D11 device adapter");
			return false;
		}
		if (!CreateDeviceOnD3D11Adapter() || !CreateInterop())
			return false;
		renderThread = std::this_thread::get_id();
		available.store(true, std::memory_order_release);
		if (!DX12LightCulling::Get().Initialize(*this)) {
			available.store(false, std::memory_order_release);
			SetDiagnostic(CS_DX12_E_RUNTIME_UNAVAILABLE, "Failed to initialize the CS clustered-lighting contributor");
			return false;
		}
		static std::once_flag shutdownRegistered;
		std::call_once(shutdownRegistered, [] { std::atexit([] { DX12RenderRuntime::Get().Shutdown(); }); });
		SetDiagnostic(CS_DX12_OK, "DX12 graph runtime initialized on the D3D11 adapter");
		logger::info("[DX12RenderRuntime] BasicRHI D3D12 device and graphics/compute/copy queues initialized on Skyrim's D3D11 adapter");
		logger::info("[DX12RenderRuntime] OpenRenderGraph host initialized and queue registry set up");
		return true;
	} catch (...) {
		SetDiagnostic(CS_DX12_E_INTERNAL, "Exception while initializing DX12 graph runtime");
		logger::error("[DX12RenderRuntime] Exception while initializing BasicRHI/OpenRenderGraph");
		return false;
	}
}

bool DX12RenderRuntime::CreateDeviceOnD3D11Adapter() noexcept
{
	rhi::DeviceCreateInfo createInfo{};
	createInfo.backend = rhi::Backend::D3D12;
	createInfo.framesInFlight = kCommandFrameCount;
	createInfo.enableDebug = false;
	createInfo.nativeAdapter = adapter.get();
	if (rhi::Failed(rhi::CreateD3D12Device(createInfo, rhiDevice)) || !rhiDevice) {
		SetDiagnostic(CS_DX12_E_RUNTIME_UNAVAILABLE, "BasicRHI failed to create D3D12 on the D3D11 adapter");
		return false;
	}
	rhiGraphicsQueue = rhiDevice->GetQueue(rhi::QueueKind::Graphics);
	rhiComputeQueue = rhiDevice->GetQueue(rhi::QueueKind::Compute);
	rhiCopyQueue = rhiDevice->GetQueue(rhi::QueueKind::Copy);
	auto* nativeDevice = rhi::dx12::get_device(rhiDevice.Get());
	auto* nativeGraphics = rhi::dx12::get_queue(rhiGraphicsQueue);
	auto* nativeCompute = rhi::dx12::get_queue(rhiComputeQueue);
	auto* nativeCopy = rhi::dx12::get_queue(rhiCopyQueue);
	if (!nativeDevice || !nativeGraphics || !nativeCompute || !nativeCopy) {
		SetDiagnostic(CS_DX12_E_RUNTIME_UNAVAILABLE, "BasicRHI did not expose the required native queues");
		return false;
	}
	device12.copy_from(nativeDevice);
	graphicsQueue.copy_from(nativeGraphics);
	computeQueue.copy_from(nativeCompute);
	copyQueue.copy_from(nativeCopy);
	renderGraph = DX12GraphHost::Create(rhiDevice.Get());
	return true;
}

bool DX12RenderRuntime::CreateInterop() noexcept
{
	auto createPair = [&](rhi::TimelinePtr& timeline, winrt::com_ptr<ID3D12Fence>& fence12, winrt::com_ptr<ID3D11Fence>& fence11, const char* name) {
		if (rhi::Failed(rhiDevice->CreateTimeline(timeline, 0, name, true)) || !timeline)
			return false;
		auto* nativeFence = rhi::dx12::get_timeline(timeline.Get());
		if (!nativeFence)
			return false;
		fence12.copy_from(nativeFence);
		HANDLE handle{};
		if (FAILED(device12->CreateSharedHandle(fence12.get(), nullptr, GENERIC_ALL, nullptr, &handle)))
			return false;
		const HRESULT result = device11->OpenSharedFence(handle, IID_PPV_ARGS(fence11.put()));
		CloseHandle(handle);
		return SUCCEEDED(result);
	};
	if (!createPair(readyTimeline, readyFence12, readyFence11, "CS D3D11 to ORG ready") ||
		!createPair(completeTimeline, completeFence12, completeFence11, "ORG to CS D3D11 complete")) {
		SetDiagnostic(CS_DX12_E_RUNTIME_UNAVAILABLE, "Failed to create D3D11/D3D12 shared timeline fences");
		return false;
	}
	return true;
}

bool DX12RenderRuntime::IsNamespaced(std::string_view id) noexcept
{
	const auto dot = id.find('.');
	return dot != std::string_view::npos && dot != 0 && dot + 1 < id.size();
}

void DX12RenderRuntime::SetDiagnostic(CSDX12Status status, std::string message) const noexcept
{
	std::scoped_lock lock(diagnosticMutex);
	diagnostic.structSize = sizeof(diagnostic);
	diagnostic.apiVersion = CS_DX12_GRAPH_API_CURRENT;
	diagnostic.status = status;
	++diagnostic.sequence;
	std::strncpy(diagnostic.message, message.c_str(), sizeof(diagnostic.message) - 1);
	diagnostic.message[sizeof(diagnostic.message) - 1] = '\0';
}

CSDX12Status DX12RenderRuntime::Register(const CSDX12ContributorDesc* desc, CSDX12RegistrationHandle* out) noexcept
{
	if (!desc || !out || !ValidHeader(desc->structSize, desc->apiVersion, sizeof(*desc)) || !desc->id || !desc->build)
		return CS_DX12_E_INVALID_ARGUMENT;
	const std::string id(desc->id);
	if (!IsNamespaced(id) || id.starts_with("cs."))
		return CS_DX12_E_RESERVED_ID;
	std::unique_lock lock(registryMutex);
	if (!registrationOpen.load())
		return CS_DX12_E_CLOSED;
	if (std::ranges::any_of(contributors, [&](const auto& entry) { return entry.second.id == id; }))
		return CS_DX12_E_DUPLICATE_ID;
	const auto handle = nextHandle.fetch_add(1);
	Contributor contributor{};
	contributor.handle = handle; contributor.id = id; contributor.kind = desc->kind; contributor.userData = desc->userData;
	contributor.build = desc->build; contributor.generationActivated = desc->generationActivated;
	contributor.generationRetired = desc->generationRetired; contributor.deviceLost = desc->deviceLost; contributor.shutdown = desc->shutdown;
	contributors.emplace(handle, std::move(contributor));
	*out = handle;
	rebuildRequested.store(true);
	return CS_DX12_OK;
}

CSDX12Status DX12RenderRuntime::Unregister(CSDX12RegistrationHandle handle) noexcept
{
	std::unique_lock lock(registryMutex);
	const auto it = contributors.find(handle);
	if (it == contributors.end())
		return CS_DX12_E_STALE_HANDLE;
	it->second.unregisterRequested = true;
	unregistering.emplace(handle, it->second);
	contributors.erase(it);
	rebuildRequested.store(true);
	return CS_DX12_OK;
}

CSDX12Status DX12RenderRuntime::FindResource(CSDX12BuildHandle build, const char* id, CSDX12ResourceHandle* out) noexcept
{
	if (std::this_thread::get_id() != renderThread) return CS_DX12_E_WRONG_THREAD;
	if (!currentBuild || currentBuild->handle != build) return CS_DX12_E_STALE_HANDLE;
	if (!id || !out) return CS_DX12_E_INVALID_ARGUMENT;
	const auto it = std::ranges::find_if(currentBuild->candidate->resources, [&](const Resource& resource) { return resource.id == id; });
	if (it == currentBuild->candidate->resources.end()) return CS_DX12_E_MISSING_DEPENDENCY;
	*out = it->handle;
	return CS_DX12_OK;
}

CSDX12Status DX12RenderRuntime::DeclareResource(CSDX12BuildHandle build, const CSDX12ResourceDesc* desc, CSDX12ResourceHandle* out) noexcept
{
	if (std::this_thread::get_id() != renderThread) return CS_DX12_E_WRONG_THREAD;
	if (!currentBuild || currentBuild->handle != build)
		return CS_DX12_E_STALE_HANDLE;
	if (!desc || !out || !ValidHeader(desc->structSize, desc->apiVersion, sizeof(*desc)) || !desc->id)
		return CS_DX12_E_INVALID_ARGUMENT;
	const std::string id(desc->id);
	if (!IsNamespaced(id) || id.starts_with("cs."))
		return CS_DX12_E_RESERVED_ID;
	if (!id.starts_with(currentBuild->contributor->id + "."))
		return CS_DX12_E_RESERVED_ID;
	if (std::ranges::any_of(currentBuild->candidate->resources, [&](const Resource& r) { return r.id == id; }))
		return CS_DX12_E_DUPLICATE_ID;
	Resource resource{};
	resource.handle = nextHandle.fetch_add(1);
	resource.id = id;
	resource.owner = currentBuild->contributor->id;
	resource.desc = *desc;
	resource.desc.id = nullptr;
	currentBuild->candidate->resources.push_back(std::move(resource));
	*out = currentBuild->candidate->resources.back().handle;
	return CS_DX12_OK;
}

CSDX12Status DX12RenderRuntime::DeclarePass(CSDX12BuildHandle build, const CSDX12PassDesc* desc, CSDX12PassHandle* out) noexcept
{
	if (std::this_thread::get_id() != renderThread) return CS_DX12_E_WRONG_THREAD;
	if (!currentBuild || currentBuild->handle != build)
		return CS_DX12_E_STALE_HANDLE;
	if (!desc || !out || !ValidHeader(desc->structSize, desc->apiVersion, sizeof(*desc)) || !desc->id || !desc->execute)
		return CS_DX12_E_INVALID_ARGUMENT;
	Pass pass{};
	pass.id = desc->id;
	if (!IsNamespaced(pass.id) || pass.id.starts_with("cs.") || !pass.id.starts_with(currentBuild->contributor->id + "."))
		return CS_DX12_E_RESERVED_ID;
	if (std::ranges::any_of(currentBuild->candidate->passes, [&](const Pass& p) { return p.id == pass.id; }))
		return CS_DX12_E_DUPLICATE_ID;
	pass.handle = nextHandle.fetch_add(1);
	if (pass.queuePolicy < CS_DX12_QUEUE_AUTOMATIC || pass.queuePolicy > CS_DX12_QUEUE_REQUIRE_COPY)
		return CS_DX12_E_INVALID_ARGUMENT;
	pass.owner = currentBuild->contributor->id;
	pass.queuePolicy = desc->queuePolicy;
	pass.flags = desc->flags;
	pass.execute = desc->execute;
	pass.userData = currentBuild->contributor->userData;
	for (uint32_t i = 0; i < desc->afterCount; ++i) if (desc->after && desc->after[i]) pass.after.emplace_back(desc->after[i]);
	for (uint32_t i = 0; i < desc->beforeCount; ++i) if (desc->before && desc->before[i]) pass.before.emplace_back(desc->before[i]);
	for (uint32_t i = 0; i < desc->accessCount; ++i) {
		if (!desc->accesses || !std::ranges::any_of(currentBuild->candidate->resources, [&](const Resource& r) { return r.handle == desc->accesses[i].resource; }))
			return CS_DX12_E_MISSING_DEPENDENCY;
		pass.accesses.push_back(desc->accesses[i]);
	}
	currentBuild->candidate->passes.push_back(std::move(pass));
	*out = currentBuild->candidate->passes.back().handle;
	return CS_DX12_OK;
}

CSDX12Status DX12RenderRuntime::RequestRebuild(CSDX12RegistrationHandle handle) noexcept
{
	std::shared_lock lock(registryMutex);
	if (!contributors.contains(handle))
		return CS_DX12_E_STALE_HANDLE;
	rebuildRequested.store(true);
	return CS_DX12_OK;
}

bool DX12RenderRuntime::Rebuild() noexcept
{
	std::vector<Contributor> snapshot;
	{
		std::shared_lock lock(registryMutex);
		for (const auto& [_, contributor] : contributors) snapshot.push_back(contributor);
	}
	std::ranges::sort(snapshot, {}, &Contributor::id);
	auto candidate = std::make_unique<Generation>();
	candidate->id = nextGeneration.fetch_add(1);
	candidate->contributorSnapshot = snapshot;
	for (const auto& contributor : snapshot) {
		BuildState build{ nextHandle.fetch_add(1), &contributor, candidate.get() };
		currentBuild = &build;
		CSDX12Status result = CS_DX12_E_CALLBACK_FAILED;
		try { result = contributor.build(contributor.userData, build.handle); } catch (...) { result = CS_DX12_E_CALLBACK_FAILED; }
		currentBuild = nullptr;
		if (result != CS_DX12_OK) {
			candidate->passes.erase(std::remove_if(candidate->passes.begin(), candidate->passes.end(), [&](const Pass& p) { return p.owner == contributor.id; }), candidate->passes.end());
			candidate->resources.erase(std::remove_if(candidate->resources.begin(), candidate->resources.end(), [&](const Resource& r) { return r.owner == contributor.id; }), candidate->resources.end());
			if (contributor.kind == CS_DX12_CONTRIBUTOR_REQUIRED) {
				SetDiagnostic(result, "Required contributor failed: " + contributor.id);
				return false;
			}
		}
	}

	std::unordered_map<std::string, size_t> node;
	for (auto anchor : kAnchors) node.emplace(anchor, node.size());
	for (const auto& pass : candidate->passes) node.emplace(pass.id, node.size());
	std::vector<std::vector<size_t>> edges(node.size());
	std::vector<uint32_t> indegree(node.size());
	for (size_t i = 1; i < std::size(kAnchors); ++i) { edges[i - 1].push_back(i); ++indegree[i]; }
	auto addEdge = [&](std::string_view from, std::string_view to) {
		const auto a = node.find(std::string(from)), b = node.find(std::string(to));
		if (a == node.end() || b == node.end()) return false;
		edges[a->second].push_back(b->second); ++indegree[b->second]; return true;
	};
	for (const auto& pass : candidate->passes) {
		for (const auto& after : pass.after) if (!addEdge(after, pass.id)) { SetDiagnostic(CS_DX12_E_MISSING_DEPENDENCY, "Missing dependency: " + after); return false; }
		for (const auto& before : pass.before) if (!addEdge(pass.id, before)) { SetDiagnostic(CS_DX12_E_MISSING_DEPENDENCY, "Missing dependency: " + before); return false; }
	}
	std::priority_queue<size_t, std::vector<size_t>, std::greater<>> ready;
	for (size_t i = 0; i < indegree.size(); ++i) if (!indegree[i]) ready.push(i);
	std::vector<size_t> order;
	while (!ready.empty()) { const auto n = ready.top(); ready.pop(); order.push_back(n); for (auto v : edges[n]) if (--indegree[v] == 0) ready.push(v); }
	if (order.size() != node.size()) { SetDiagnostic(CS_DX12_E_CYCLE, "Contributor pass dependency cycle"); return false; }
	std::unordered_map<size_t, Pass> passes;
	for (auto& pass : candidate->passes) passes.emplace(node[pass.id], std::move(pass));
	candidate->passes.clear();
	for (auto index : order) if (passes.contains(index)) candidate->passes.push_back(std::move(passes[index]));

	std::vector<DX12GraphHost::WorkItem> workItems;
	workItems.reserve(candidate->passes.size());
	for (const auto& pass : candidate->passes) {
		const auto id = pass.id;
		const auto execute = pass.execute;
		void* const userData = pass.userData;
		workItems.push_back({ id, pass.queuePolicy, pass.flags, pass.after, pass.before, [this, id, execute, userData](const CSDX12ExecutionContext& context) {
			CSDX12Status status = CS_DX12_E_CALLBACK_FAILED;
			try { status = execute(userData, &context); } catch (...) { status = CS_DX12_E_CALLBACK_FAILED; }
			if (status != CS_DX12_OK) SetDiagnostic(status, "Execution callback failed: " + id);
			return status;
		} });
	}
	try {
		renderGraph->SetStructuralWorkItems(std::move(workItems));
	} catch (const std::exception& e) {
		SetDiagnostic(CS_DX12_E_INTERNAL, std::string("ORG structural compile failed: ") + e.what());
		return false;
	}

	if (active) retired.emplace_back(active->lastCompletion, std::move(active));
	active = std::move(candidate);
	for (const auto& contributor : active->contributorSnapshot)
		if (contributor.generationActivated) contributor.generationActivated(contributor.userData, active->id);
	SetDiagnostic(CS_DX12_OK, "Activated graph generation " + std::to_string(active->id));
	return true;
}

void DX12RenderRuntime::ExecuteDeferredEpoch(uint32_t width, uint32_t height) noexcept
{
	if (!available.load() || std::this_thread::get_id() != renderThread)
		return;
	if (rebuildRequested.exchange(false) && !Rebuild()) { /* keep previous generation */ }
	if (!active)
		return;
	const uint64_t ready = nextReadyFence.fetch_add(1);
	if (FAILED(context11->Signal(readyFence11.get(), ready)))
		return;
	const uint64_t complete = nextCompleteFence.fetch_add(1);
	CSDX12FrameInfo frame{ sizeof(frame), CS_DX12_GRAPH_API_CURRENT, frameIndex, active->id, complete,
		static_cast<uint32_t>(frameIndex % kCommandFrameCount), kCommandFrameCount, width, height };
	try {
		renderGraph->Execute(static_cast<uint32_t>(frameIndex), complete, readyTimeline.Get(), ready,
			completeTimeline.Get(), complete, frame);
	} catch (const std::exception& e) {
		const HRESULT removedReason = device12 ? device12->GetDeviceRemovedReason() : S_OK;
		logger::error("[DX12RenderRuntime] ORG execution failed: {} (device reason=0x{:08X})", e.what(), static_cast<unsigned>(removedReason));
		winrt::com_ptr<ID3D12InfoQueue> infoQueue;
		if (device12 && SUCCEEDED(device12->QueryInterface(IID_PPV_ARGS(infoQueue.put())))) {
			const auto messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
			const auto firstMessage = messageCount > 32 ? messageCount - 32 : 0;
			for (uint64_t index = firstMessage; index < messageCount; ++index) {
				SIZE_T messageSize = 0;
				if (FAILED(infoQueue->GetMessage(index, nullptr, &messageSize)) || messageSize == 0) continue;
				std::vector<std::byte> storage(messageSize);
				auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
				if (SUCCEEDED(infoQueue->GetMessage(index, message, &messageSize)))
					logger::error("[DX12RenderRuntime] D3D12 debug [{}]: {}", static_cast<unsigned>(message->ID), message->pDescription);
			}
		}
		if (rhi::g_breakCallback) rhi::g_breakCallback();
		SetDiagnostic(CS_DX12_E_INTERNAL, std::string("ORG execution failed: ") + e.what());
		if (FAILED(removedReason)) {
			available.store(false);
			logger::error("[DX12RenderRuntime] D3D12 device was removed; disabling DX12 contributors for the remainder of the process");
		}
		return;
	}
	++frameIndex;
	active->lastCompletion = complete;
	lastSubmittedCompletion.store(complete, std::memory_order_release);
	context11->Wait(completeFence11.get(), complete);
	RetireCompleted();
}

void DX12RenderRuntime::RetireCompleted() noexcept
{
	const auto completed = completeFence12 ? completeFence12->GetCompletedValue() : 0;
	if (renderGraph) renderGraph->Retire(completed);
	while (!retired.empty() && retired.front().first <= completed) {
		for (const auto& contributor : retired.front().second->contributorSnapshot)
			if (contributor.generationRetired) contributor.generationRetired(contributor.userData, retired.front().second->id);
		retired.pop_front();
	}
	std::unique_lock lock(registryMutex);
	for (auto it = unregistering.begin(); it != unregistering.end();) {
		const bool referenced = (active && std::ranges::any_of(active->contributorSnapshot, [&](const Contributor& c) { return c.handle == it->first; })) ||
			std::ranges::any_of(retired, [&](const auto& generation) { return std::ranges::any_of(generation.second->contributorSnapshot, [&](const Contributor& c) { return c.handle == it->first; }); });
		if (!referenced) it = unregistering.erase(it); else ++it;
	}
}

void DX12RenderRuntime::Shutdown() noexcept
{
	registrationOpen.store(false);
	available.store(false);
	if (rhiGraphicsQueue && completeTimeline) {
		const auto value = nextCompleteFence.fetch_add(1);
		rhiGraphicsQueue.Signal({ completeTimeline->GetHandle(), value });
		if (context11 && completeFence11) context11->Wait(completeFence11.get(), value);
	}
	std::unique_lock lock(registryMutex);
	for (auto& [_, contributor] : contributors) if (contributor.shutdown) contributor.shutdown(contributor.userData);
	for (auto& [_, contributor] : unregistering) if (contributor.shutdown) contributor.shutdown(contributor.userData);
	contributors.clear(); unregistering.clear(); active.reset(); retired.clear();
	renderGraph.reset();
	readyFence11 = nullptr; readyFence12 = nullptr;
	completeFence11 = nullptr; completeFence12 = nullptr;
	readyTimeline.Reset(); completeTimeline.Reset();
	copyQueue = nullptr; computeQueue = nullptr;
	graphicsQueue = nullptr; device12 = nullptr;
	rhiGraphicsQueue = {}; rhiComputeQueue = {}; rhiCopyQueue = {}; rhiDevice.Reset();
	adapter = nullptr; context11 = nullptr; device11 = nullptr;
}

CSDX12Status DX12RenderRuntime::GetRuntimeInfo(CSDX12RuntimeInfo* out) const noexcept
{
	if (!out || out->structSize < sizeof(*out)) return CS_DX12_E_INVALID_ARGUMENT;
	uint64_t capabilities = CS_DX12_CAP_ASYNC_COMPUTE | CS_DX12_CAP_COPY_QUEUE | CS_DX12_CAP_NATIVE_ESCAPE_HATCH;
#if defined(CS_HAS_ORG_MODULE_SERVICES)
	capabilities |= CS_DX12_CAP_FRAME_UPLOADS | CS_DX12_CAP_MODULE_SERVICES;
#endif
	*out = { sizeof(*out), CS_DX12_GRAPH_API_CURRENT, available.load() ? 1u : 0u, 0u,
		capabilities,
		active ? active->id : 0u, lastSubmittedCompletion.load(), completeFence12 ? completeFence12->GetCompletedValue() : 0u,
		kCommandFrameCount, 0u };
	return CS_DX12_OK;
}

CSDX12Status DX12RenderRuntime::IsRegistrationRetired(CSDX12RegistrationHandle handle, uint32_t* out) const noexcept
{
	if (!out) return CS_DX12_E_INVALID_ARGUMENT;
	std::shared_lock lock(registryMutex);
	if (contributors.contains(handle)) { *out = 0; return CS_DX12_OK; }
	if (unregistering.contains(handle)) { *out = 0; return CS_DX12_OK; }
	*out = 1; return CS_DX12_OK;
}

CSDX12Status DX12RenderRuntime::GetLastDiagnostic(CSDX12RegistrationHandle handle, CSDX12Diagnostic* out) const noexcept
{
	if (!out || out->structSize < sizeof(*out)) return CS_DX12_E_INVALID_ARGUMENT;
	std::scoped_lock lock(diagnosticMutex); *out = diagnostic; out->contributor = handle; return CS_DX12_OK;
}

namespace
{
	CSDX12Status CS_DX12_GRAPH_CALL ApiRuntimeInfo(CSDX12RuntimeInfo* out) { return DX12RenderRuntime::Get().GetRuntimeInfo(out); }
	CSDX12Status CS_DX12_GRAPH_CALL ApiDevice(void** out) { if (!out) return CS_DX12_E_INVALID_ARGUMENT; *out = DX12RenderRuntime::Get().GetNativeDevice(); return *out ? CS_DX12_OK : CS_DX12_E_RUNTIME_UNAVAILABLE; }
	CSDX12Status CS_DX12_GRAPH_CALL ApiRegister(const CSDX12ContributorDesc* d, CSDX12RegistrationHandle* h) { return DX12RenderRuntime::Get().Register(d, h); }
	CSDX12Status CS_DX12_GRAPH_CALL ApiUnregister(CSDX12RegistrationHandle h) { return DX12RenderRuntime::Get().Unregister(h); }
	CSDX12Status CS_DX12_GRAPH_CALL ApiResource(CSDX12BuildHandle b, const CSDX12ResourceDesc* d, CSDX12ResourceHandle* h) { return DX12RenderRuntime::Get().DeclareResource(b, d, h); }
	CSDX12Status CS_DX12_GRAPH_CALL ApiFindResource(CSDX12BuildHandle b, const char* id, CSDX12ResourceHandle* h) { return DX12RenderRuntime::Get().FindResource(b, id, h); }
	CSDX12Status CS_DX12_GRAPH_CALL ApiPass(CSDX12BuildHandle b, const CSDX12PassDesc* d, CSDX12PassHandle* h) { return DX12RenderRuntime::Get().DeclarePass(b, d, h); }
	CSDX12Status CS_DX12_GRAPH_CALL ApiRebuild(CSDX12RegistrationHandle h) { return DX12RenderRuntime::Get().RequestRebuild(h); }
	CSDX12Status CS_DX12_GRAPH_CALL ApiRetired(CSDX12RegistrationHandle h, uint32_t* retired) { return DX12RenderRuntime::Get().IsRegistrationRetired(h, retired); }
	CSDX12Status CS_DX12_GRAPH_CALL ApiDiagnostic(CSDX12RegistrationHandle h, CSDX12Diagnostic* d) { return DX12RenderRuntime::Get().GetLastDiagnostic(h, d); }
	const char* CS_DX12_GRAPH_CALL ApiStatusString(CSDX12Status status) {
		switch (status) {
		case CS_DX12_OK: return "success"; case CS_DX12_E_INVALID_ARGUMENT: return "invalid argument";
		case CS_DX12_E_UNSUPPORTED_VERSION: return "unsupported API version"; case CS_DX12_E_RUNTIME_UNAVAILABLE: return "runtime unavailable";
		case CS_DX12_E_STALE_HANDLE: return "stale handle"; case CS_DX12_E_DUPLICATE_ID: return "duplicate identifier";
		case CS_DX12_E_RESERVED_ID: return "reserved identifier"; case CS_DX12_E_MISSING_DEPENDENCY: return "missing dependency";
		case CS_DX12_E_CYCLE: return "dependency cycle"; case CS_DX12_E_INCOMPATIBLE_RESOURCE: return "incompatible resource";
		case CS_DX12_E_WRONG_THREAD: return "wrong thread"; case CS_DX12_E_CALLBACK_FAILED: return "callback failed";
		case CS_DX12_E_CLOSED: return "registration closed"; case CS_DX12_E_NOT_READY: return "not ready";
		case CS_DX12_E_UNSUPPORTED_CAPABILITY: return "unsupported capability"; default: return "internal error";
		}
	}
}

extern "C" CSDX12Status CS_DX12_GRAPH_CALL CS_GetDX12GraphAPI(uint32_t version, CSDX12GraphAPI* out)
{
	if (!out || out->structSize < sizeof(CSDX12GraphAPI)) {
		return CS_DX12_E_INVALID_ARGUMENT;
	}
	if (version != CS_DX12_GRAPH_API_CURRENT) {
		return CS_DX12_E_UNSUPPORTED_VERSION;
	}
	*out = { sizeof(*out), version, &ApiRuntimeInfo, &ApiDevice, &ApiRegister, &ApiUnregister, &ApiResource,
		&ApiFindResource, &ApiPass, &ApiRebuild, &ApiRetired, &ApiDiagnostic, &ApiStatusString };
	return CS_DX12_OK;
}

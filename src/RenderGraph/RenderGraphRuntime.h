#pragma once

#include <CommunityShaders/RenderGraphAPI.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <winrt/base.h>
#include <rhi.h>
#include "RenderGraphHost.h"
#include "D3D11InteropBridge.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class RenderGraphRuntime
{
public:
	static RenderGraphRuntime& Get();

	bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context) noexcept;
	bool ExecuteGraph(uint32_t width, uint32_t height, uint32_t allocationWidth, uint32_t allocationHeight) noexcept;
	void Shutdown() noexcept;
	bool IsAvailable() const noexcept { return available.load(std::memory_order_acquire); }
	rhi::Device GetRHIDevice() const noexcept { return rhiDevice ? rhiDevice.Get() : rhi::Device{}; }
	D3D11InteropBridge* GetInteropCoordinator() const noexcept { return interopCoordinator.get(); }
	uint32_t GetRenderWidth() const noexcept { return renderWidth; }
	uint32_t GetRenderHeight() const noexcept { return renderHeight; }
	uint32_t GetAllocationWidth() const noexcept { return allocationWidth; }
	uint32_t GetAllocationHeight() const noexcept { return allocationHeight; }
	org::services::ShaderCompiler* GetShaderCompiler() noexcept { return renderGraph ? renderGraph->GetShaderCompiler() : nullptr; }
	org::services::PipelineService* GetPipelineService() noexcept { return renderGraph ? renderGraph->GetPipelineService() : nullptr; }
	void RequestGraphRebuild() noexcept { rebuildRequested.store(true, std::memory_order_release); }

	uint64_t GetActiveGeneration() const noexcept;
	uint64_t GetLastSubmittedCompletion() const noexcept { return lastSubmittedCompletion.load(std::memory_order_acquire); }
	uint64_t GetCompletedGraphValue() noexcept { return completeTimeline ? completeTimeline->GetCompletedValue() : 0; }
	static constexpr uint32_t GetFramesInFlight() noexcept { return kCommandFrameCount; }

private:
	struct Generation;

	RenderGraphRuntime() = default;
	~RenderGraphRuntime() = default;
	RenderGraphRuntime(const RenderGraphRuntime&) = delete;
	RenderGraphRuntime& operator=(const RenderGraphRuntime&) = delete;

	bool CreateDeviceOnD3D11Adapter() noexcept;
	bool CreateInterop() noexcept;
	bool Rebuild() noexcept;
	void RetireCompleted() noexcept;
	void SetDiagnostic(CSRGStatus status, std::string message) const noexcept;

	winrt::com_ptr<ID3D11Device5> device11;
	winrt::com_ptr<ID3D11DeviceContext4> context11;
	winrt::com_ptr<IDXGIAdapter> adapter;
	rhi::DevicePtr rhiDevice;
	rhi::Queue rhiGraphicsQueue{};
	rhi::Queue rhiComputeQueue{};
	rhi::Queue rhiCopyQueue{};
	std::unique_ptr<RenderGraphHost> renderGraph;
	std::unique_ptr<D3D11InteropBridge> interopCoordinator;
	rhi::TimelinePtr readyTimeline;
	rhi::TimelinePtr completeTimeline;
	winrt::com_ptr<ID3D11Fence> readyFence11;
	winrt::com_ptr<ID3D11Fence> completeFence11;
	static constexpr uint32_t kCommandFrameCount = 3;

	std::atomic_bool available{ false };
	std::atomic_bool rebuildRequested{ true };
	std::atomic_uint64_t nextReadyFence{ 1 };
	std::atomic_uint64_t nextCompleteFence{ 1 };
	std::atomic_uint64_t nextGeneration{ 1 };
	std::atomic_uint64_t lastSubmittedCompletion{};
	uint64_t frameIndex = 0;
	uint32_t renderWidth{};
	uint32_t renderHeight{};
	uint32_t allocationWidth{};
	uint32_t allocationHeight{};
	std::thread::id renderThread;

	mutable std::shared_mutex registryMutex;
	std::unique_ptr<Generation> active;
	std::deque<std::pair<uint64_t, std::unique_ptr<Generation>>> retired;

	mutable std::mutex diagnosticMutex;
	mutable CSRGStatus diagnosticStatus{ CS_RG_OK };
	mutable uint64_t diagnosticSequence{};
	mutable std::string diagnosticMessage;
};

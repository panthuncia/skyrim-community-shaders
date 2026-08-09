#pragma once

#include <CommunityShaders/DX12GraphAPI.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>
#include <rhi.h>
#include "DX12GraphHost.h"
#include "DX12InteropCoordinator.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class DX12RenderRuntime
{
public:
	static DX12RenderRuntime& Get();

	bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context) noexcept;
	bool ExecuteDeferredEpoch(uint32_t width, uint32_t height, uint32_t allocationWidth, uint32_t allocationHeight) noexcept;
	void Shutdown() noexcept;
	bool IsAvailable() const noexcept { return available.load(std::memory_order_acquire); }
	ID3D12Device* GetNativeDevice() const noexcept { return device12.get(); }
	ID3D12CommandQueue* GetGraphicsQueue() const noexcept { return graphicsQueue.get(); }
	ID3D11Device5* GetD3D11Device() const noexcept { return device11.get(); }
	DX12InteropCoordinator* GetInteropCoordinator() const noexcept { return interopCoordinator.get(); }
	uint32_t GetRenderWidth() const noexcept { return renderWidth; }
	uint32_t GetRenderHeight() const noexcept { return renderHeight; }
	uint32_t GetAllocationWidth() const noexcept { return allocationWidth; }
	uint32_t GetAllocationHeight() const noexcept { return allocationHeight; }
	org::services::ShaderCompiler* GetShaderCompiler() noexcept { return renderGraph ? renderGraph->GetShaderCompiler() : nullptr; }
	org::services::PipelineService* GetPipelineService() noexcept { return renderGraph ? renderGraph->GetPipelineService() : nullptr; }

	CSDX12Status GetRuntimeInfo(CSDX12RuntimeInfo* out) const noexcept;
	CSDX12Status Register(const CSDX12ContributorDesc* desc, CSDX12RegistrationHandle* out) noexcept;
	CSDX12Status Unregister(CSDX12RegistrationHandle handle) noexcept;
	CSDX12Status DeclareResource(CSDX12BuildHandle build, const CSDX12ResourceDesc* desc, CSDX12ResourceHandle* out) noexcept;
	CSDX12Status FindResource(CSDX12BuildHandle build, const char* id, CSDX12ResourceHandle* out) noexcept;
	CSDX12Status DeclarePass(CSDX12BuildHandle build, const CSDX12PassDesc* desc, CSDX12PassHandle* out) noexcept;
	CSDX12Status RequestRebuild(CSDX12RegistrationHandle handle) noexcept;
	CSDX12Status IsRegistrationRetired(CSDX12RegistrationHandle handle, uint32_t* out) const noexcept;
	CSDX12Status GetLastDiagnostic(CSDX12RegistrationHandle handle, CSDX12Diagnostic* out) const noexcept;

private:
	struct Contributor;
	struct Resource;
	struct Pass;
	struct Generation;
	struct BuildState;

	DX12RenderRuntime() = default;
	~DX12RenderRuntime() = default;
	DX12RenderRuntime(const DX12RenderRuntime&) = delete;
	DX12RenderRuntime& operator=(const DX12RenderRuntime&) = delete;

	bool CreateDeviceOnD3D11Adapter() noexcept;
	bool CreateInterop() noexcept;
	bool Rebuild() noexcept;
	void RetireCompleted() noexcept;
	void SetDiagnostic(CSDX12Status status, std::string message) const noexcept;
	static bool IsNamespaced(std::string_view id) noexcept;

	winrt::com_ptr<ID3D11Device5> device11;
	winrt::com_ptr<ID3D11DeviceContext4> context11;
	winrt::com_ptr<IDXGIAdapter> adapter;
	rhi::DevicePtr rhiDevice;
	rhi::Queue rhiGraphicsQueue{};
	rhi::Queue rhiComputeQueue{};
	rhi::Queue rhiCopyQueue{};
	std::unique_ptr<DX12GraphHost> renderGraph;
	winrt::com_ptr<ID3D12Device> device12;
	winrt::com_ptr<ID3D12CommandQueue> graphicsQueue;
	winrt::com_ptr<ID3D12CommandQueue> computeQueue;
	winrt::com_ptr<ID3D12CommandQueue> copyQueue;
	std::unique_ptr<DX12InteropCoordinator> interopCoordinator;
	rhi::TimelinePtr readyTimeline;
	rhi::TimelinePtr completeTimeline;
	winrt::com_ptr<ID3D12Fence> readyFence12;
	winrt::com_ptr<ID3D11Fence> readyFence11;
	winrt::com_ptr<ID3D12Fence> completeFence12;
	winrt::com_ptr<ID3D11Fence> completeFence11;
	static constexpr uint32_t kCommandFrameCount = 3;

	std::atomic_bool available{ false };
	std::atomic_bool registrationOpen{ true };
	std::atomic_bool rebuildRequested{ true };
	std::atomic_uint64_t nextHandle{ 1 };
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
	std::unordered_map<uint64_t, Contributor> contributors;
	std::unordered_map<uint64_t, Contributor> unregistering;
	std::unique_ptr<Generation> active;
	std::deque<std::pair<uint64_t, std::unique_ptr<Generation>>> retired;
	BuildState* currentBuild = nullptr;

	mutable std::mutex diagnosticMutex;
	mutable CSDX12Diagnostic diagnostic{};
};
